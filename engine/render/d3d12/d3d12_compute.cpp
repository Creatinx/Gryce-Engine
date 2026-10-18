#include "d3d12_compute.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "d3d12_backend.h"
#include "d3d12_descriptors.h"
#include "d3d12_dxc.h"
#include "d3d12_spirv_cross.h"
#include "d3d12_vulkanize.h"
#include "render/render.h"
#include "render/shader_source_resolver.h"
#include "render/glsl_compiler.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

D3D12ComputeShader::~D3D12ComputeShader() {
    if (pso_) { pso_->Release(); pso_ = nullptr; }
    if (root_signature_) { root_signature_->Release(); root_signature_ = nullptr; }
}

void D3D12ComputeShader::set_context(D3D12Backend* backend, ID3D12Device* device) {
    backend_ = backend;
    device_ = device;
}

// ---------------------------------------------------------------------------
// 根签名（计算版）
// ---------------------------------------------------------------------------
// 与图形版的差别（这就是为什么不能共用一份）：
//   * **不带** ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT（计算管线没有 IA）；
//   * 多一个 **UAV 描述符表**（u0..uM-1）；
//   * 可见性统一 ALL（计算只有一种可见性）。
//
// 描述符表的规则与图形一致：表内第 k 个槽位 ↔ 寄存器 t{k} / u{k}，范围必须覆盖
// HLSL 里出现的最大寄存器（由反射给出，不能猜 —— 猜小了会越界访问描述符堆）。
bool D3D12ComputeShader::build_root_signature(uint32_t srv_count, uint32_t uav_count) {
    if (root_signature_) { root_signature_->Release(); root_signature_ = nullptr; }

    D3D12_ROOT_PARAMETER params[4]{};
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    uint32_t param_count = 0;

    if (srv_count > 0) {
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = srv_count;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        params[param_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[param_count].DescriptorTable.NumDescriptorRanges = 1;
        params[param_count].DescriptorTable.pDescriptorRanges = &ranges[0];
        params[param_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++param_count;
    }

    if (uav_count > 0) {
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = uav_count;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        params[param_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[param_count].DescriptorTable.NumDescriptorRanges = 1;
        params[param_count].DescriptorTable.pDescriptorRanges = &ranges[1];
        params[param_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++param_count;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = param_count;
    desc.pParameters = param_count ? params : nullptr;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // 计算：无 IA 布局

    PfnD3D12SerializeRootSignature serialize = backend_->loader().serialize_root_signature;
    if (!serialize) {
        GLOG_ERROR("D3D12ComputeShader '{}': D3D12SerializeRootSignature unavailable", name_);
        return false;
    }
    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "(no detail)";
        GLOG_ERROR("D3D12ComputeShader '{}': serialize root signature failed 0x{:08X}: {}",
                   name_, static_cast<uint32_t>(hr), msg);
        if (err) err->Release();
        if (sig) sig->Release();
        return false;
    }
    hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      __mingw_uuidof<ID3D12RootSignature>(),
                                      reinterpret_cast<void**>(&root_signature_));
    sig->Release();
    if (err) err->Release();
    if (FAILED(hr) || !root_signature_) {
        GLOG_ERROR("D3D12ComputeShader '{}': CreateRootSignature failed 0x{:08X}",
                   name_, static_cast<uint32_t>(hr));
        root_signature_ = nullptr;
        return false;
    }
    return true;
}

bool D3D12ComputeShader::create_pso(const std::vector<uint8_t>& dxil) {
    if (!root_signature_ || dxil.empty()) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature_;
    desc.CS.pShaderBytecode = dxil.data();
    desc.CS.BytecodeLength = dxil.size();
    desc.NodeMask = 0;
    desc.CachedPSO.pCachedBlob = nullptr;
    desc.CachedPSO.CachedBlobSizeInBytes = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HRESULT hr = device_->CreateComputePipelineState(
        &desc, __mingw_uuidof<ID3D12PipelineState>(), reinterpret_cast<void**>(&pso_));
    if (FAILED(hr) || !pso_) {
        GLOG_ERROR("D3D12ComputeShader '{}': CreateComputePipelineState failed 0x{:08X}",
                   name_, static_cast<uint32_t>(hr));
        pso_ = nullptr;
        return false;
    }
    return true;
}

bool D3D12ComputeShader::load(const std::string& name, const std::string& shader_dir) {
    if (!backend_ || !device_) {
        GLOG_ERROR("D3D12ComputeShader::load({}): no device context", name);
        return false;
    }
    name_ = name;
    shader_dir_ = shader_dir;

    // ---- 1) 读源码 ----
    ShaderStageSource src = resolve_shader_stage_source(name, shader_dir, ".comp", RenderAPI::DX12);
    if (!src.ok()) {
        GLOG_WARN("D3D12ComputeShader '{}': compute source not found (dir='{}')", name, shader_dir);
        return false;
    }

    // ---- 2) GL 方言 → Vulkan 方言 ----
    VulkanizedShader vz = vulkanize_glsl(src.code, GlslVariant::Compute);
    if (!vz.ok) {
        GLOG_ERROR("D3D12ComputeShader '{}': vulkanize failed: {}", name, vz.error);
        return false;
    }

    // ---- 3) Vulkan-GLSL → SPIR-V ----
    std::vector<uint32_t> spirv;
    std::string err;
    if (!compile_glsl_to_spirv(vz.source, "d3d12_compute.comp", GlslStage::Compute, spirv, err)) {
        GLOG_ERROR("D3D12ComputeShader '{}': shaderc failed: {}", name, err);
        return false;
    }

    // ---- 4) SPIR-V → HLSL ----
    std::string hlsl;
    if (!spirv_to_hlsl(spirv, SpvcStage::Compute, "main", {}, hlsl, err)) {
        GLOG_ERROR("D3D12ComputeShader '{}': spirv-cross failed: {}", name, err);
        return false;
    }
    if (const char* dump_dir = std::getenv("GRYCE_D3D12_DUMP_HLSL")) {
        std::ofstream f(std::string(dump_dir) + "/" + name + ".comp.hlsl", std::ios::binary);
        f << hlsl;
    }

    // ---- 5) 反射 SSBO 绑定（块名 → 寄存器 / UAV 标志 / 元素步长）----
    refl_ = reflect_ssbo_bindings(hlsl, vz.source);
    for (const SsboBinding& b : refl_.bindings) {
        GLOG_INFO("D3D12ComputeShader '{}': SSBO block='{}' hlsl='{}' type='{}' reg={}{} "
                  "{} stride={}",
                  name, b.block, b.hlsl_var, b.hlsl_type, b.is_uav ? "u" : "t", b.reg,
                  b.is_raw ? "raw" : "structured", b.stride);
    }

    // ---- 6) HLSL → DXIL ----
    if (!compile_hlsl_to_dxil(hlsl, "main", DxcProfile::Cs_6_0, dxil_, err)) {
        GLOG_ERROR("D3D12ComputeShader '{}': dxc failed: {}", name, err);
        return false;
    }

    // ---- 7) 根签名 + PSO ----
    if (!build_root_signature(refl_.srv_count, refl_.uav_count)) return false;
    if (!create_pso(dxil_)) return false;

    GLOG_INFO("D3D12ComputeShader '{}': ready (srv={}, uav={})",
              name, refl_.srv_count, refl_.uav_count);
    return true;
}

void D3D12ComputeShader::bind_storage(const char* name, ID3D12Resource* res, uint64_t size_bytes) {
    if (!name || !res) return;
    pending_[name] = Binding{res, size_bytes};
    if (!find_ssbo(refl_.bindings, name) && !warned_missing_block_) {
        warned_missing_block_ = true;
        GLOG_ERROR("D3D12ComputeShader '{}': no SSBO block named '{}' in the compiled shader "
                   "(block names are case-sensitive; using a member name instead of the block "
                   "name compiles fine but never binds)", name_, name);
    }
}

bool D3D12ComputeShader::dispatch(ID3D12GraphicsCommandList* cmd,
                                  uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!cmd || !pso_ || !root_signature_) return false;

    DescriptorHeapSet& heaps = backend_->descriptor_heaps();
    if (!heaps.is_valid()) return false;

    // 描述符表按寄存器号填充：表内第 k 槽 ↔ register t{k} / u{k}。
    const uint32_t srv_count = refl_.srv_count;
    const uint32_t uav_count = refl_.uav_count;

    if ((srv_count > 0 || uav_count > 0) && !tables_allocated_) {
        if (srv_count > 0 && !heaps.srv().allocate_range(srv_count, srv_table_base_)) {
            GLOG_ERROR("D3D12ComputeShader '{}': SRV table alloc failed ({} slots)", name_, srv_count);
            return false;
        }
        if (uav_count > 0 && !heaps.srv().allocate_range(uav_count, uav_table_base_)) {
            GLOG_ERROR("D3D12ComputeShader '{}': UAV table alloc failed ({} slots)", name_, uav_count);
            return false;
        }
        tables_allocated_ = true;
    }

    // ---- 写 SRV / UAV 描述符 ----
    for (const SsboBinding& b : refl_.bindings) {
        auto it = pending_.find(b.block);
        if (it == pending_.end()) {
            // 没绑定的块：写一个 null 描述符（读返回 0，不触发校验）。
            continue;
        }
        ID3D12Resource* res = it->second.res;
        const uint64_t bytes = it->second.size;
        const uint32_t stride = b.stride ? b.stride : 4u;
        // ★ raw（ByteAddressBuffer）与 structured 的视图描述符**完全不同**：
        //   raw   → R32_TYPELESS + StructureByteStride=0 + *_FLAG_RAW + NumElements=size/4
        //   structured → UNKNOWN + StructureByteStride=stride + FLAG_NONE
        //   弄反了着色器声明的布局与描述符不符 → 设备被移除（0x887A0005）。
        const uint32_t elements = static_cast<uint32_t>(bytes / (b.is_raw ? 4u : stride));

        if (b.is_uav) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format = b.is_raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = 0;
            d.Buffer.NumElements = elements;
            d.Buffer.StructureByteStride = b.is_raw ? 0u : stride;
            d.Buffer.CounterOffsetInBytes = 0;
            d.Buffer.Flags = b.is_raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
            device_->CreateUnorderedAccessView(res, nullptr, &d,
                                              heaps.srv().cpu_handle(uav_table_base_ + b.reg));
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = b.is_raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = 0;
            d.Buffer.NumElements = elements;
            d.Buffer.StructureByteStride = b.is_raw ? 0u : stride;
            d.Buffer.Flags = b.is_raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
            device_->CreateShaderResourceView(res, &d,
                                             heaps.srv().cpu_handle(srv_table_base_ + b.reg));
        }
    }

    // 描述符堆必须处于绑定状态（计算派发发生在主渲染目标绑定之前）。
    heaps.bind(cmd);
    cmd->SetComputeRootSignature(root_signature_);

    uint32_t param = 0;
    if (srv_count > 0) {
        cmd->SetComputeRootDescriptorTable(param++, heaps.srv().gpu_handle(srv_table_base_));
    }
    if (uav_count > 0) {
        cmd->SetComputeRootDescriptorTable(param++, heaps.srv().gpu_handle(uav_table_base_));
    }
    cmd->SetPipelineState(pso_);
    cmd->Dispatch(gx, gy, gz);
    return true;
}

} // namespace gryce_engine::render::d3d12
