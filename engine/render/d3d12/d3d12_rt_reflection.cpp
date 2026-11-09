// d3d12_rt_reflection.cpp — DXR inline 光追反射实现（见头文件设计说明）。
#include "d3d12_rt_reflection.h"

#include <cstring>

#include "d3d12_backend.h"
#include "d3d12_texture.h"
#include "d3d12_dxc.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

namespace {

// 与 HLSL cbuffer 逐字段对应（列主序矩阵直接 memcpy）。
struct RtCB {
    float inv_view_proj[16];
    float view_proj[16];
    float cam_pos_max_dist[4];
    float params[4];   // x=strength, y=envIntensity, z=hasNormal, w=unused
    float dims[4];     // xy=resolution
};

const char* k_rt_hlsl = R"HLSL(
cbuffer U : register(b0)
{
    float4x4 invViewProj;
    float4x4 viewProj;
    float4 camPosMaxDist;   // xyz = camera pos, w = max trace distance
    float4 params;          // x = strength, y = envIntensity, z = hasNormal, w = hasPrefilter
    float4 dims;            // xy = full resolution, z = 预过滤立方体的最粗 mip
};

Texture2D<float>    tDepth  : register(t0);
Texture2D<float4>   tLit    : register(t1);
Texture2D<float4>   tNormal : register(t2);
TextureCube<float4> tEnv    : register(t3);
RaytracingAccelerationStructure tlas : register(t4);
TextureCube<float4> tEnvPre : register(t5);
RWTexture2D<float4> outColor : register(u0);
SamplerState samp : register(s0);

float3 reconstructWorld(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 w = mul(invViewProj, clip);
    return w.xyz / w.w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= (uint)dims.x || px.y >= (uint)dims.y) return;

    float4 lit = tLit.Load(int3(px, 0));
    // 无法线预通道时反射方向不可信，直接 passthrough（与数据缺失时的安全降级一致）。
    if (params.z < 0.5) { outColor[px] = float4(lit.rgb, 1.0); return; }

    float depth = tDepth.Load(int3(px, 0));
    if (depth >= 1.0) { outColor[px] = float4(lit.rgb, 1.0); return; }

    float2 uv = (float2(px) + 0.5) / dims.xy;
    float3 camPos = camPosMaxDist.xyz;
    float3 world = reconstructWorld(uv, depth);
    float3 vdir = normalize(world - camPos);

    float3 n = normalize(tNormal.Load(int3(px, 0)).rgb * 2.0 - 1.0);
    if (dot(n, vdir) > 0.0) n = -n;

    float rho = saturate(lit.a);
    float envInt = params.y;
    float3 reflCol = 0.0;
    float cover = 0.0;

    if (rho > 0.02)
    {
        float3 rdir = reflect(vdir, n);
        RayDesc rd;
        rd.Origin = world + n * 0.02;
        rd.Direction = rdir;
        rd.TMin = 0.02;
        rd.TMax = camPosMaxDist.w;

        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, rd);
        while (q.Proceed()) {}

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            float t = q.CommittedRayT();
            float3 hit = rd.Origin + rdir * t;
            float4 hc = mul(viewProj, float4(hit, 1.0));
            bool onScreen = false;
            if (hc.w > 0.0)
            {
                float2 huv = (hc.xy / hc.w) * 0.5 + 0.5;
                huv.y = 1.0 - huv.y;
                if (all(huv >= 0.0) && all(huv <= 1.0))
                {
                    // 遮挡校验：屏幕该像素必须真是命中面，否则采到的是更近的遮挡物
                    // （漏光根因）。线性距离相对差 > 6% 视为被遮挡，回退环境。
                    int2 hp = clamp(int2(huv * dims.xy), int2(0, 0), int2(dims.xy) - 1);
                    float sz = tDepth.Load(int3(hp, 0));
                    float hitLin = length(hit - camPos);
                    if (sz < 1.0)
                    {
                        float sceneLin = length(reconstructWorld(huv, sz) - camPos);
                        float rel = abs(sceneLin - hitLin) / max(hitLin, 1e-3);
                        if (rel < 0.06)
                        {
                            reflCol = tLit.Load(int3(hp, 0)).rgb;
                            onScreen = true;
                        }
                    }
                }
            }
            if (onScreen) cover = 1.0;
            else if (envInt > 0.0) {
                // 命中点在屏幕外：该点没被着色过，采不到屏幕色。用**预过滤立方体的
                // 最粗 mip**（约等于全方向平均辐射）兜底 —— 封闭空间里按方向采锐利
                // 天空会在镜面上凭空画出一个室外天空，比一块平稳环境色更离谱。
                reflCol = params.w > 0.5 ? tEnvPre.SampleLevel(samp, rdir, dims.z).rgb
                                         : tEnv.SampleLevel(samp, rdir, 0).rgb;
                reflCol *= envInt;
                cover = 1.0;
            }
        }
        else if (envInt > 0.0)
        {
            reflCol = tEnv.SampleLevel(samp, rdir, 0).rgb * envInt;
            cover = 1.0;
        }
    }

    // 与 SSR 同口径：cover*ρ 加权；反射色取自被反射内容本身，只按反射面自身
    // 色相轻微染色（亮度守恒，反射强度仍完全由 w 决定）。
    float lum = dot(lit.rgb, float3(0.2126, 0.7152, 0.0722));
    float3 hue = lum > 1e-4 ? lit.rgb / lum : float3(1.0, 1.0, 1.0);
    float refl_lum = dot(reflCol, float3(0.2126, 0.7152, 0.0722));
    float w = saturate(cover * rho * params.x);
    float3 refl_col = lerp(reflCol, hue * refl_lum, 0.35);
    float3 color = lerp(lit.rgb, refl_col, w);
    outColor[px] = float4(color, 1.0);
}
)HLSL";

} // namespace

bool D3D12RtReflection::init(D3D12Backend* backend) {
    if (valid_) return true;
    if (!backend) return false;
    backend_ = backend;
    ID3D12Device* dev = backend->device();
    if (!dev) return false;

    // 设备 5：DXR 接口。
    if (FAILED(dev->QueryInterface(__mingw_uuidof<ID3D12Device5>(),
                                   reinterpret_cast<void**>(&device5_))) ||
        !device5_) {
        GLOG_INFO("D3D12RT: ID3D12Device5 unavailable; DXR reflection disabled");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))) ||
        opts5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        GLOG_INFO("D3D12RT: raytracing tier < 1.1 (inline raytracing unsupported); disabled");
        device5_->Release();
        device5_ = nullptr;
        return false;
    }

    if (!dxc_available()) {
        GLOG_WARN("D3D12RT: DXC unavailable; DXR reflection disabled");
        device5_->Release();
        device5_ = nullptr;
        return false;
    }

    // 输出图先占位（渲染时按尺寸创建）。
    if (!create_root_signature()) {
        shutdown();
        return false;
    }

    // 描述符区间：SRV t0..t4 连续 5 个；UAV u0 1 个。
    DescriptorAllocator& srv_alloc = backend_->descriptor_heaps().srv();
    if (!srv_alloc.allocate_range(6, srv_base_) ||
        !srv_alloc.allocate_range(1, uav_base_)) {
        GLOG_ERROR("D3D12RT: descriptor range allocation failed");
        shutdown();
        return false;
    }

    // 每帧 uniform（256B，UPLOAD 持久映射）。
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                __mingw_uuidof<ID3D12Resource>(),
                                                reinterpret_cast<void**>(&cb_))) ||
            !cb_) {
            shutdown();
            return false;
        }
        cb_->SetName(L"rt_reflection_cb");
        cb_->Map(0, nullptr, &cb_mapped_);
    }

    // 兜底立方体（无环境时占位；envIntensity=0 时着色器不会采样它）。
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 1;
        rd.Height = 1;
        rd.DepthOrArraySize = 6;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        // mingw 的 d3d12.h 没有 D3D12_RESOURCE_MISC_TEXTURECUBE（SDK 里值就是 0x4），
        // 直接按 SDK 数值填 Flags，避免为这一个宏引入 SDK 头依赖。
        rd.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x4);
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                __mingw_uuidof<ID3D12Resource>(),
                                                reinterpret_cast<void**>(&fallback_cube_))) ||
            !fallback_cube_) {
            shutdown();
            return false;
        }
        fallback_cube_->SetName(L"rt_reflection_fallback_cube");
    }

    if (!create_pipeline()) {
        shutdown();
        return false;
    }

    valid_ = true;
    GLOG_INFO("D3D12RT: DXR inline reflection enabled (tier 1.1)");
    return true;
}

void D3D12RtReflection::shutdown() {
    auto safe_release = [](ID3D12Resource*& r) {
        if (r) { r->Release(); r = nullptr; }
    };
    blas_cache_.clear();
    tlas_.destroy();
    destroy_output();
    safe_release(fallback_cube_);
    safe_release(cb_);
    cb_mapped_ = nullptr;
    if (pso_) { pso_->Release(); pso_ = nullptr; }
    if (root_) { root_->Release(); root_ = nullptr; }
    if (device5_) { device5_->Release(); device5_ = nullptr; }
    backend_ = nullptr;
    valid_ = false;
}

bool D3D12RtReflection::create_root_signature() {
    D3D12_ROOT_PARAMETER params[3]{};
    D3D12_DESCRIPTOR_RANGE ranges[2]{};

    // 0: CBV b0
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 1: SRV table t0..t4
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 6;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 2: UAV table u0
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MipLODBias = 0.0f;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samp.MinLOD = 0.0f;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.RegisterSpace = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = backend_->loader().serialize_root_signature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr) || !sig) {
        GLOG_ERROR("D3D12RT: root signature serialize failed 0x{:08X}", (uint32_t)hr);
        if (err) err->Release();
        return false;
    }
    hr = backend_->device()->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __mingw_uuidof<ID3D12RootSignature>(), reinterpret_cast<void**>(&root_));
    sig->Release();
    if (err) err->Release();
    if (FAILED(hr) || !root_) {
        GLOG_ERROR("D3D12RT: CreateRootSignature failed 0x{:08X}", (uint32_t)hr);
        return false;
    }
    return true;
}

bool D3D12RtReflection::create_pipeline() {
    std::vector<uint8_t> dxil;
    std::string error;
    if (!compile_hlsl_to_dxil(k_rt_hlsl, "main", DxcProfile::Cs_6_5, dxil, error)) {
        GLOG_WARN("D3D12RT: HLSL compile failed: {}", error);
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_;
    desc.CS.pShaderBytecode = dxil.data();
    desc.CS.BytecodeLength = dxil.size();
    desc.NodeMask = 0;
    if (FAILED(backend_->device()->CreateComputePipelineState(
            &desc, __mingw_uuidof<ID3D12PipelineState>(),
            reinterpret_cast<void**>(&pso_))) ||
        !pso_) {
        GLOG_ERROR("D3D12RT: CreateComputePipelineState failed");
        return false;
    }
    return true;
}

bool D3D12RtReflection::ensure_output(int width, int height) {
    if (out_ && out_w_ == width && out_h_ == height) return true;
    destroy_output();
    ID3D12Device* dev = backend_->device();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = static_cast<uint64_t>(width);
    rd.Height = static_cast<uint32_t>(height);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            __mingw_uuidof<ID3D12Resource>(),
                                            reinterpret_cast<void**>(&out_))) ||
        !out_) {
        return false;
    }
    out_->SetName(L"rt_reflection_out");
    out_w_ = width;
    out_h_ = height;
    return true;
}

void D3D12RtReflection::destroy_output() {
    if (out_) {
        // ★ 必须走后端的延迟释放队列。
        //   这张 UAV 输出图仍被"已提交但未完成"的命令列表引用（本帧的 trace/
        //   composite 就在用它，上一帧的 CopyResource 也可能没跑完）。直接
        //   Release 会让列表关闭时报 "resource deleted prior to closing the
        //   command list"，驱动侧则直接 DXGI_ERROR_DEVICE_HUNG。
        //   窗口一旦改变尺寸（每帧 width/height 都变），这里必然被触发 ——
        //   表现就是"拖动窗口缩放，画面卡死/设备移除"。
        if (backend_) {
            backend_->retire_gpu_resource(out_);
        } else {
            out_->Release();
        }
        out_ = nullptr;
    }
    out_w_ = out_h_ = 0;
}

void D3D12RtReflection::render(ID3D12GraphicsCommandList* cmd,
                               D3D12Texture* color, D3D12Texture* depth, D3D12Texture* normal,
                               D3D12Texture* env, D3D12Texture* env_prefilter,
                               int width, int height,
                               const math::Matrix4f& view_proj,
                               const math::Matrix4f& inv_view_proj,
                               const math::Vector3f& camera_pos,
                               const PostProcessParams& pp,
                               const std::vector<RtDrawInstance>& instances) {
    if (!valid_ || !cmd || !color || !depth || instances.empty()) return;
    if (width <= 0 || height <= 0) return;
    if (!ensure_output(width, height)) return;

    ID3D12GraphicsCommandList4* cmd4 = nullptr;
    if (FAILED(cmd->QueryInterface(__mingw_uuidof<ID3D12GraphicsCommandList4>(),
                                   reinterpret_cast<void**>(&cmd4))) ||
        !cmd4) {
        return;
    }

    // ---- 1) BLAS / TLAS ----
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
    descs.reserve(instances.size());
    for (const auto& inst : instances) {
        IMesh* imesh = inst.mesh;
        if (!imesh) continue;
        const float* pos = nullptr;
        const uint32_t* idx = nullptr;
        uint32_t vcount = 0, icount = 0;
        if (!imesh->get_ray_tracing_geometry(pos, vcount, idx, icount)) continue;
        if (!pos || !idx || vcount == 0 || icount == 0) continue;

        auto it = blas_cache_.find(imesh);
        if (it == blas_cache_.end()) {
            D3D12BottomLevelAS blas;
            RtGeometry geo{pos, vcount, idx, icount};
            if (!blas.build(device5_, cmd4, geo)) continue;
            it = blas_cache_.emplace(imesh, std::move(blas)).first;
        }
        D3D12BottomLevelAS& blas = it->second;
        if (!blas.valid()) continue;

        D3D12_RAYTRACING_INSTANCE_DESC d{};
        const math::Matrix4f& t = inst.transform;
        d.Transform[0][0] = t.m[0]; d.Transform[0][1] = t.m[4]; d.Transform[0][2] = t.m[8];
        d.Transform[0][3] = t.m[12];
        d.Transform[1][0] = t.m[1]; d.Transform[1][1] = t.m[5]; d.Transform[1][2] = t.m[9];
        d.Transform[1][3] = t.m[13];
        d.Transform[2][0] = t.m[2]; d.Transform[2][1] = t.m[6]; d.Transform[2][2] = t.m[10];
        d.Transform[2][3] = t.m[14];
        d.InstanceID = static_cast<uint32_t>(descs.size()) & 0xFFFFFF;
        d.InstanceMask = 0xFF;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure = blas.gpu_address();
        descs.push_back(d);
    }
    cmd4->Release();
    if (descs.empty()) return;

    if (!tlas_.ensure_capacity(device5_, static_cast<uint32_t>(descs.size()))) return;
    if (!tlas_.update_instances(descs.data(), descs.size() * sizeof(descs[0]))) return;

    ID3D12GraphicsCommandList4* cmd4b = nullptr;
    if (FAILED(cmd->QueryInterface(__mingw_uuidof<ID3D12GraphicsCommandList4>(),
                                   reinterpret_cast<void**>(&cmd4b))) ||
        !cmd4b) {
        return;
    }
    bool build_ok = tlas_.record_build(device5_, cmd4b, static_cast<uint32_t>(descs.size()),
                                      false);
    cmd4b->Release();
    if (!build_ok) return;

    // ---- 2) 资源状态 → compute 可采样 ----
    auto transition = [&](D3D12Texture* tex, D3D12_RESOURCE_STATES after) {
        if (!tex) return;
        D3D12_RESOURCE_STATES before = tex->current_state();
        if (before == after) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->resource();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
        tex->set_current_state(after);
    };
    const D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(color, kSrv);
    transition(depth, kSrv);
    if (normal) transition(normal, kSrv);
    // 环境 cube 在像素着色阶段被 IBL 采样，这里要把它并到 compute 也能读的态。
    if (env) {
        transition(env, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    D3D12_RESOURCE_STATES out_before = D3D12_RESOURCE_STATE_COMMON;
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out_;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = out_before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &b);
    }

    // ---- 3) uniform ----
    {
        RtCB cb{};
        std::memcpy(cb.inv_view_proj, inv_view_proj.m, sizeof(cb.inv_view_proj));
        std::memcpy(cb.view_proj, view_proj.m, sizeof(cb.view_proj));
        cb.cam_pos_max_dist[0] = camera_pos.x;
        cb.cam_pos_max_dist[1] = camera_pos.y;
        cb.cam_pos_max_dist[2] = camera_pos.z;
        cb.cam_pos_max_dist[3] = pp.rt_reflection_max_distance > 0.0f
                                     ? pp.rt_reflection_max_distance
                                     : 100.0f;
        cb.params[0] = 1.0f;
        cb.params[1] = (env && pp.env_intensity > 0.0f) ? pp.env_intensity : 0.0f;
        cb.params[2] = normal ? 1.0f : 0.0f;
        cb.params[3] = env_prefilter ? 1.0f : 0.0f;
        cb.dims[0] = static_cast<float>(width);
        cb.dims[1] = static_cast<float>(height);
        // 预过滤立方体的最粗 mip（≈ 全方向平均辐射），出屏命中时按它兜底。
        int pre_mips = env_prefilter ? env_prefilter->mip_levels() : 0;
        cb.dims[2] = static_cast<float>(pre_mips > 0 ? pre_mips - 1 : 0);
        cb.dims[3] = 0.0f;
        if (cb_mapped_) std::memcpy(cb_mapped_, &cb, sizeof(cb));
    }

    // ---- 4) 描述符 ----
    ID3D12Device* dev = backend_->device();
    DescriptorAllocator& srv_alloc = backend_->descriptor_heaps().srv();
    auto make_tex2d_srv = [&](uint32_t idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
        dev->CreateShaderResourceView(res, &srv,         srv_alloc.cpu_handle(srv_base_ + idx));
    };
    make_tex2d_srv(0, depth->resource(), DXGI_FORMAT_R32_FLOAT);
    make_tex2d_srv(1, color->resource(), color->dxgi_format());
    ID3D12Resource* normal_res = normal ? normal->resource() : color->resource();
    DXGI_FORMAT normal_fmt = normal ? normal->dxgi_format() : color->dxgi_format();
    make_tex2d_srv(2, normal_res, normal_fmt);

    // t3: 环境 cube（无环境绑兜底，envIntensity=0 时不采样）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = env ? env->dxgi_format() : DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = env ? env->mip_levels() : 1;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
        dev->CreateShaderResourceView(env ? env->resource() : fallback_cube_, &srv,
                                      srv_alloc.cpu_handle(srv_base_ + 3));
    }

    // t4: TLAS
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.RaytracingAccelerationStructure.Location = tlas_.gpu_address();
        dev->CreateShaderResourceView(nullptr, &srv, srv_alloc.cpu_handle(srv_base_ + 4));
    }

    // t5: 预过滤环境 cube（无则绑兜底，params.w=0 时着色器不采它）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = env_prefilter ? env_prefilter->dxgi_format() : DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = env_prefilter ? env_prefilter->mip_levels() : 1;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
        dev->CreateShaderResourceView(env_prefilter ? env_prefilter->resource() : fallback_cube_,
                                      &srv, srv_alloc.cpu_handle(srv_base_ + 5));
    }

    // u0: 输出
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = 0;
        uav.Texture2D.PlaneSlice = 0;
        dev->CreateUnorderedAccessView(out_, nullptr, &uav,
                                       srv_alloc.cpu_handle(uav_base_));
    }

    // ---- 5) 绑 ----
    backend_->descriptor_heaps().bind(cmd);
    cmd->SetComputeRootSignature(root_);
    cmd->SetComputeRootConstantBufferView(0, cb_->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, srv_alloc.gpu_handle(srv_base_));
    cmd->SetComputeRootDescriptorTable(2, srv_alloc.gpu_handle(uav_base_));
    cmd->SetPipelineState(pso_);
    cmd->Dispatch(static_cast<UINT>((width + 7) / 8), static_cast<UINT>((height + 7) / 8), 1);

    // ---- 6) 输出 → HDR ----
    {
        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_;
        cmd->ResourceBarrier(1, &uav_bar);
    }
    transition(color, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out_;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1, &b);
    }
    cmd->CopyResource(color->resource(), out_);

    // 后续 pass 会采样颜色与深度：切回 SRV 态。
    transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (normal) {
        transition(normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // out_ 下帧开头从 COMMON 起（这里保持 COPY_SOURCE，下帧 transition 用 COMMON 前置）。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out_;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cmd->ResourceBarrier(1, &b);
    }
    ++frame_index_;
}

} // namespace gryce_engine::render::d3d12
