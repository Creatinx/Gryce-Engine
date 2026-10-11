// =============================================================================
// d3d12_shader.cpp — IShader 的 D3D12 实现（M4.4）
//
// 实现细节与踩坑记录（都是实测结论，不是推测）：
//
// ---------------------------------------------------------------------------
// 1) 为什么 CBV 用 UploadRing 而不是每 shader 一块持久 mapped 缓冲
// ---------------------------------------------------------------------------
// 同一个 shader 在一帧内会被画多次，**每次的 uniform 值都不同**（不同材质、
// 不同物体）。如果 CBV 指向同一块内存，后续 draw 会覆盖前面 draw 还没读到的值
// —— 这正是 Vulkan 侧 vk_shader.cpp 里 post_process 要"每 draw 分配独立描述符集"
// 的原因（见该文件 1454 行的注释）。
//
// 所以 commit_uniforms() 每次都从 UploadRing 申请新的一段，把影子内存拷进去，
// 返回新的 GPU 地址。UploadRing 在本帧内单向推进，天然不会互相覆盖。
//
// ---------------------------------------------------------------------------
// 2) D3D12 的 CBV 大小必须 256 对齐
// ---------------------------------------------------------------------------
// D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes 必须是 256 的倍数。std140 块的
// 自然大小不满足（例如 scene_pbr.frag 是 864 字节 → 补齐到 1024）。UploadRing
// 的 allocate(size) 已经按 256 对齐，我们只要把 SizeInBytes 传对齐后的值。
// 传未对齐值不会立刻报错，而是在 Close() 时炸成 E_INVALIDARG —— 定位极痛苦，
// 所以这里显式 align_up。
//
// ---------------------------------------------------------------------------
// 3) SPIRV-Cross 的 HLSL 输出里 sampler 的寄存器编号
// ---------------------------------------------------------------------------
// vulkanize 给 sampler 分配 `layout(set=0, binding=1..N)`。SPIRV-Cross 转 HLSL
// 时会**保留 binding 号的相对次序**并重编号为 `register(t0..tN-1)`（因为它把
// set=0 的 binding 视为 HLSL 的 t 空间，从 0 起顺排）。
// 实测 d3d12_scene_pbr.frag.hlsl 里 uAlbedo/uNormal/... 就是 `register(t0)`、
// `register(t1)` … 按声明顺序。
// 因此 SRV 表索引 = sampler_names_ 的下标，不需要任何 slot 映射表
// （这也解释了为什么 D3D12 不需要像 GL 那样做 texture_unit_for_slot 重映射）。
//
// ---------------------------------------------------------------------------
// 4) 深度约定与 Y 翻转在翻译层完成，不在引擎数学里
// ---------------------------------------------------------------------------
// SPIRV-Cross 的 FIXUP_DEPTH_CONVENTION + FLIP_VERTEX_Y 让生成的 main() 末尾
// 多出：
//     gl_Position.y = -gl_Position.y;
//     gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
// 于是 D3D12 的 near→0 / far→1 约定成立，Matrix4f::perspective 与
// math_camera CTest 断言都不需要改（CTest 5/5 保持全绿）。
// =============================================================================

#include "d3d12_shader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

#include "d3d12_backend.h"
#include "d3d12_mesh.h"
#include "d3d12_texture.h"
#include "d3d12_spirv_cross.h"
#include "d3d12_dxc.h"
#include "render/mesh.h"
#include "render/render.h"
#include "render/shader_source_resolver.h"
#include "render/vulkan/vk_glsl_compiler.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

namespace {

// 扫 HLSL 里 `register(<letter>N)` 出现的最大 N，返回 N+1（一个都没出现返回 0）。
// 用于决定 root signature 里 SRV / Sampler 描述符表的范围大小。
uint32_t max_register_in_hlsl(const std::string& hlsl, char letter) {
    const std::string key = std::string("register(") + letter;
    uint32_t best = 0;
    for (size_t p = hlsl.find(key); p != std::string::npos; p = hlsl.find(key, p + 1)) {
        size_t q = p + key.size();
        uint32_t v = 0;
        bool any = false;
        while (q < hlsl.size() && std::isdigit(static_cast<unsigned char>(hlsl[q]))) {
            v = v * 10u + static_cast<uint32_t>(hlsl[q] - '0');
            ++q;
            any = true;
        }
        if (any && v + 1 > best) best = v + 1;
    }
    return best;
}

// VertexType → DXGI_FORMAT。
DXGI_FORMAT dxgi_format_for_vertex_type(VertexType t) {
    switch (t) {
        case VertexType::Float:   return DXGI_FORMAT_R32_FLOAT;
        case VertexType::Float2:  return DXGI_FORMAT_R32G32_FLOAT;
        case VertexType::Float3:  return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexType::Float4:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VertexType::Int:     return DXGI_FORMAT_R32_SINT;
        case VertexType::UInt:    return DXGI_FORMAT_R32_UINT;
        case VertexType::Int4:    return DXGI_FORMAT_R32G32B32A32_SINT;
        case VertexType::UInt4:   return DXGI_FORMAT_R32G32B32A32_UINT;
    }
    return DXGI_FORMAT_R32G32B32_FLOAT;
}

// 输入布局的分类：按范围插入 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA / _PER_INSTANCE_DATA。
// 引擎约定 location 5..8 同时被"蒙皮骨骼"和"实例 mat4"占用，靠**布局来源**
// （vertex_layout vs instance_layout）区分，不靠 location 范围猜
// （见 d3d12_mesh.h 的说明）。

D3D12_CULL_MODE d3d12_cull_from(uint8_t cull) {
    switch (cull) {
        case 0: return D3D12_CULL_MODE_BACK;
        case 1: return D3D12_CULL_MODE_NONE;
        case 2: return D3D12_CULL_MODE_FRONT;
        default: return D3D12_CULL_MODE_BACK;
    }
}

D3D12_BLEND to_d3d12_blend(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return D3D12_BLEND_ZERO;
        case BlendFactor::One:              return D3D12_BLEND_ONE;
        case BlendFactor::SrcAlpha:         return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcColor:         return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::DstColor:         return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP to_d3d12_blend_op(BlendEquation e) {
    switch (e) {
        case BlendEquation::Add:             return D3D12_BLEND_OP_ADD;
        case BlendEquation::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
        // ★ MinGW 的 d3d12.h 用的是缩写名 REV_SUBTRACT（MS 头里是
        //   REVERSE_SUBTRACT，两者数值相同，但符号名不同 → 必须用 MinGW 的名字）。
        case BlendEquation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendEquation::Min:             return D3D12_BLEND_OP_MIN;
        case BlendEquation::Max:             return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

} // namespace

// ---------------------------------------------------------------------------
// 布局指纹（公开：D3D12Backend 在 draw 路径构造 PsoKey 用）
// ---------------------------------------------------------------------------
// 把 (location, type, offset) 打包成整数序列。不用 hash 直接当键，是为了避免
// 碰撞导致"两个不同布局共用一条 PSO"。
//
// ★ 实例属性也要进指纹：实例化的 mesh（location 5..8 为 PER_INSTANCE 的 mat4）
//   与骨骼蒙皮的 mesh（location 5..6 为 BLENDINDICES/BLENDWEIGHT）在顶点布局上
//   可能完全一致，但 INPUT_LAYOUT 的 classification 不同 → 必须区分。
//   用 0xFFFFFFFF 哨兵分隔顶点段与实例段。
constexpr uint32_t kLayoutKeySentinel = 0xFFFFFFFFu;

VertexLayoutKey make_layout_key(const VertexLayout& layout, const VertexLayout& instance_layout) {
    VertexLayoutKey k;
    k.stride = layout.stride;
    k.packed.reserve(layout.attributes.size() * 3 + 4);
    for (const VertexAttribute& a : layout.attributes) {
        k.packed.push_back(a.location);
        k.packed.push_back(static_cast<uint32_t>(a.type));
        k.packed.push_back(a.offset);
    }
    // 哨兵 + 实例布局指纹（stride 与逐属性的 location/type/offset）。
    k.packed.push_back(kLayoutKeySentinel);
    if (instance_layout.stride > 0 && !instance_layout.attributes.empty()) {
        k.packed.push_back(instance_layout.stride);
        for (const VertexAttribute& a : instance_layout.attributes) {
            k.packed.push_back(a.location);
            k.packed.push_back(static_cast<uint32_t>(a.type));
            k.packed.push_back(a.offset);
        }
    } else {
        k.packed.push_back(0);
    }
    return k;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

D3D12Shader::~D3D12Shader() {
    for (auto& entry : pso_cache_) {
        if (entry.second) entry.second->Release();
    }
    pso_cache_.clear();
    pso_ = nullptr;
    if (root_signature_) {
        root_signature_->Release();
        root_signature_ = nullptr;
    }
}

void D3D12Shader::set_context(D3D12Backend* backend, ID3D12Device* device) {
    backend_ = backend;
    device_ = device;
}

// ---------------------------------------------------------------------------
// 着色器链：GLSL → vulkanize → shaderc(SPIR-V) → SPIRV-Cross(HLSL) → DXC(DXIL)
// ---------------------------------------------------------------------------
bool D3D12Shader::build_stage(const std::string& glsl_source, GlslVariant variant,
                              std::vector<uint8_t>& out_dxil, std::string& error) {
    // ---- 第 1 步：GL 方言 → Vulkan 方言（纯文本改写，不动原文件）----
    VulkanizedShader vz = vulkanize_glsl(glsl_source, variant);
    if (!vz.ok) {
        error = "vulkanize failed: " + vz.error;
        return false;
    }
    last_vz_ = vz;   // 交给 compile() 建设该阶段的 uniform 状态

    // 顶点阶段的输入语义记录在**任一次**改写结果上（vert 才会有）。
    if (variant == GlslVariant::Vertex) {
        vertex_input_semantics_ = vz.vertex_input_semantics;
        vertex_input_locations_ = vz.vertex_input_locations;
    }

    // ---- 第 2 步：GLSL → SPIR-V ----
    GlslStage stage = (variant == GlslVariant::Vertex)     ? GlslStage::Vertex
                      : (variant == GlslVariant::Fragment) ? GlslStage::Fragment
                                                           : GlslStage::Compute;
    std::vector<uint32_t> spirv;
    std::string shaderc_err;
    const char* tag = (variant == GlslVariant::Vertex) ? "d3d12_shader.vert" : "d3d12_shader.frag";
    if (!compile_glsl_to_spirv(vz.source, tag, stage, spirv, shaderc_err)) {
        error = "shaderc failed: " + shaderc_err;
        return false;
    }

    // ---- 第 3 步：SPIR-V → HLSL ----
    SpvcStage spvc_stage = (variant == GlslVariant::Vertex)     ? SpvcStage::Vertex
                           : (variant == GlslVariant::Fragment) ? SpvcStage::Fragment
                                                                : SpvcStage::Compute;

    // ★ 顶点属性语义重映射。SPIRV-Cross 默认按 SPIR-V **变量名** 生成 HLSL 语义
    //   （aPos → TEXCOORD0），而 D3D12 输入布局按语义名匹配 → 不重映射会让顶点
    //   属性全落空（画面全黑）。这里传入 vulkanize 记录的 location → 语义名。
    std::vector<VertexSemantic> remaps;
    if (variant == GlslVariant::Vertex) {
        const size_t n = vertex_input_locations_.size();
        for (size_t i = 0; i < n && i < vertex_input_semantics_.size(); ++i) {
            VertexSemantic vs;
            vs.location = vertex_input_locations_[i];
            // 语义索引：TEXCOORD / COLOR / TANGENT / BLENDINDICES 需要后缀数字。
            // POSITION / NORMAL 的索引恒为 0，不加后缀。
            const std::string& base = vertex_input_semantics_[i];
            const char* idx = vertex_semantic_index_for_location(vertex_input_locations_[i]);
            if (base == "POSITION" || base == "NORMAL" || base == "BINORMAL") {
                vs.semantic = base;
            } else {
                vs.semantic = base + (idx ? idx : "0");
            }
            remaps.push_back(std::move(vs));
        }
    }

    std::string hlsl, spvc_err;
    if (!spirv_to_hlsl(spirv, spvc_stage, "main", remaps, hlsl, spvc_err)) {
        error = "spirv-cross failed: " + spvc_err;
        return false;
    }

    // 调试开关：GRYCE_D3D12_DUMP_HLSL=<dir> 时把 SPIRV-Cross 生成的 HLSL 落盘。
    // 整条 D3D12 着色器链是 GLSL→SPIR-V→HLSL→DXIL，中间那层 HLSL 是唯一能
    // 直接核对"翻译层到底做了什么"的产物（Y 翻转 / 深度重映射 / 语义名 /
    // cbuffer 偏移 / 寄存器号），排查渲染异常时先看它，比读汇编快得多。
    // 默认关闭，零开销（只在设置了环境变量时才做文件 IO）。
    if (const char* dump_dir = std::getenv("GRYCE_D3D12_DUMP_HLSL")) {
        std::ofstream f(std::string(dump_dir) + "/" + debug_name_ + "_" +
                            (variant == GlslVariant::Vertex ? "vert" : "frag") + ".hlsl",
                        std::ios::binary);
        f << hlsl;
    }

    // ★ 扫生成的 HLSL 取该阶段用到的最大 t/s 寄存器号。
    //   不能想当然用"采样器个数"当范围：vulkanize 的 binding 从 1 起，
    //   SPIRV-Cross 原样保留寄存器号 → N 个采样器实际占 t1..tN（不是 t0..tN-1）。
    //   而且 readonly SSBO 也会占 t（instanced.vert 的 dst_models）。
    //   这里取 max+1 才是真正覆盖全部引用的范围上界。
    srv_register_count_ = std::max(srv_register_count_, max_register_in_hlsl(hlsl, 't'));
    sampler_register_count_ = std::max(sampler_register_count_, max_register_in_hlsl(hlsl, 's'));

    // ★ 反射 SSBO 块（块名 → t/u 寄存器 / UAV 标志 / 元素步长）。
    //   图形侧真正用到的是 `instanced.vert` 的 DstModels（GPU 剔除后的实例矩阵，
    //   由 bind_instance_stream → set_external_buffer_srv 绑成 SRV）。
    {
        SsboReflection refl = reflect_ssbo_bindings(hlsl, vz.source);
        for (SsboBinding& b : refl.bindings) {
            // 同一块可能同时出现在 VS/FS（本工程不会），去重后追加。
            bool dup = false;
            for (const SsboBinding& e : ssbo_bindings_) {
                if (e.block == b.block && e.reg == b.reg && e.is_uav == b.is_uav) { dup = true; break; }
            }
            if (!dup) ssbo_bindings_.push_back(b);
        }
    }

    // ---- 第 4 步：HLSL → DXIL ----
    DxcProfile profile = (variant == GlslVariant::Vertex) ? DxcProfile::Vs_6_0
                        : (variant == GlslVariant::Fragment) ? DxcProfile::Ps_6_0
                                                             : DxcProfile::Cs_6_0;
    std::string dxc_err;
    if (!compile_hlsl_to_dxil(hlsl, "main", profile, out_dxil, dxc_err)) {
        error = "dxc failed: " + dxc_err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// compile(vert_src, frag_src) —— 便捷接口
// ---------------------------------------------------------------------------
bool D3D12Shader::compile(const std::string& vertex_src, const std::string& fragment_src) {
    if (!device_) {
        GLOG_ERROR("D3D12Shader::compile: no device context");
        return false;
    }

    // 每次 compile 重置元数据（热重载会重复调用）。
    sampler_names_.clear();
    vertex_input_semantics_.clear();
    vertex_input_locations_.clear();
    ssbo_bindings_.clear();
    external_srv_.clear();
    srv_register_count_ = 0;
    sampler_register_count_ = 0;
    has_vs_uniforms_ = false;
    has_ps_uniforms_ = false;
    vs_u_ = StageUniforms{};
    ps_u_ = StageUniforms{};

    std::string err;
    if (!build_stage(vertex_src, GlslVariant::Vertex, vs_dxil_, err)) {
        GLOG_ERROR("D3D12Shader::compile ({}): vertex stage: {}", debug_name_, err);
        valid_ = false;
        return false;
    }
    // ★ 每个阶段各自建立自己的 Globals 块状态。
    //   两个阶段的偏移**互不相同**（实测 pbr.vert 的 uModel@c0 vs
    //   pbr.frag 的 uAlbedoColor@c0），共用一个 CBV 会让 VS 把 frag 的
    //   第一个字段当 uModel 读 → 静默错位。所以这里分开建，draw 时下发两个 CBV
    //   （b0 的 VERTEX / PIXEL 两个可见性，寄存器不重叠）。
    setup_stage_uniforms(vs_u_, last_vz_);
    has_vs_uniforms_ = vs_u_.present;
    // 顶点阶段采样器：本工程 GL 着色器的 .vert 里没有 sampler（已核查），
    // 若将来出现则与 frag 的 t 寄存器编号会撞车，这里显式告警而不是静默错。
    if (!last_vz_.sampler_names.empty()) {
        GLOG_WARN("D3D12Shader '{}': vertex stage declares {} sampler(s); "
                  "vertex-stage samplers are not wired yet",
                  debug_name_, last_vz_.sampler_names.size());
    }

    if (!fragment_src.empty()) {
        if (!build_stage(fragment_src, GlslVariant::Fragment, fs_dxil_, err)) {
            GLOG_ERROR("D3D12Shader::compile ({}): fragment stage: {}", debug_name_, err);
            valid_ = false;
            return false;
        }
        setup_stage_uniforms(ps_u_, last_vz_);
        has_ps_uniforms_ = ps_u_.present;
        sampler_names_ = last_vz_.sampler_names;
    }

    bound_textures_.assign(sampler_names_.size(), BoundTexture{});

    // ---- root signature ----
    // SRV / Sampler 描述符表的范围由"HLSL 里实际出现的最大寄存器号"决定
    // （build_stage 已经扫好）。描述符表本身总是存在的，即使计数为 0
    // —— 计数为 0 时不留 root parameter，所以这里允许 0。
    if (!create_root_signature(srv_register_count_, sampler_register_count_)) {
        GLOG_ERROR("D3D12Shader::compile ({}): root signature creation failed", debug_name_);
        valid_ = false;
        return false;
    }

    valid_ = true;
    GLOG_INFO("D3D12Shader '{}': vs={}B fs={}B cb[vs]={}B cb[ps]={}B srv(t0..t{}) smp(s0..s{})",
              debug_name_, vs_dxil_.size(), fs_dxil_.size(), vs_u_.block_size, ps_u_.block_size,
              srv_register_count_ ? srv_register_count_ - 1 : 0,
              sampler_register_count_ ? sampler_register_count_ - 1 : 0);
    return true;
}

// ---------------------------------------------------------------------------
// 单个阶段的 Globals 块状态：查表 + 影子内存 + 默认值快照
// ---------------------------------------------------------------------------
void D3D12Shader::setup_stage_uniforms(StageUniforms& su, const VulkanizedShader& vz) {
    su = StageUniforms{};
    if (!vz.ok || vz.uniform_fields.empty()) return;

    su.present = true;
    su.block_size = vz.uniform_block_size;

    for (const auto& f : vz.uniform_fields) {
        UniformEntry e;
        e.offset = f.offset;
        e.size = f.size;
        e.array_count = f.array_count;
        // std140 的数组元素步长 = align_up(align_up(sizeof(T), align(T)), 16)。
        // 这里 f.size 是"整个数组的字节数"，单元素步长不能直接用 f.size
        // （只有 count==1 时才相等）。用 (size / count) 反推即可：
        // vulkanize 算 f.size 时就是 stride * n。
        e.stride = (f.array_count > 0) ? (f.size / f.array_count) : f.size;
        su.table.emplace(f.name, e);
    }

    // 影子内存按块大小 256 对齐分配（CBV 的 SizeInBytes 必须 256 的倍数）。
    const size_t aligned = (su.block_size + 255u) & ~size_t(255u);
    su.shadow.assign(aligned, 0);

    // ★ 应用 GLSL 的 uniform 初始化式。
    //   `uniform float uExposure = 1.0;` 搬进 uniform 块后成员不能带初始化式，
    //   不求值就是 0 —— 而 GL 下 uExposure/uGamma/uSaturation 等默认 1.0，
    //   清零会让 tonemap 输出全黑/全灰。这里把表达式求值写进影子内存，
    //   并存成"每帧复位用的默认快照"（GL 的 uniform 也是跨帧保持的）。
    for (const auto& f : vz.uniform_fields) {
        if (f.initializer.empty()) continue;
        auto it = su.table.find(f.name);
        if (it == su.table.end()) continue;
        uint32_t written = 0;
        const uint32_t room =
            static_cast<uint32_t>(su.shadow.size() - it->second.offset);
        if (eval_glsl_const(f.initializer, f.std140_type, su.shadow.data() + it->second.offset,
                            room, written)) {
            continue;
        }
        GLOG_WARN("D3D12Shader '{}': cannot evaluate uniform initializer '{} {} = {}' (left as 0)",
                  debug_name_, f.std140_type, f.name, f.initializer);
    }
    su.defaults = su.shadow;
}

bool D3D12Shader::compile(const std::vector<ShaderStageDesc>& stages) {
    const std::string* vert = nullptr;
    const std::string* frag = nullptr;
    for (const auto& s : stages) {
        if (s.stage == ShaderStage::Vertex) vert = &s.source;
        else if (s.stage == ShaderStage::Fragment) frag = &s.source;
    }
    if (!vert) {
        GLOG_ERROR("D3D12Shader::compile(stages): no vertex stage");
        return false;
    }
    return compile(*vert, frag ? *frag : std::string());
}

// ---------------------------------------------------------------------------
// load_program —— 从 shader_dir 读源码并编译
// ---------------------------------------------------------------------------
bool D3D12Shader::load_program(const std::string& name, const std::string& shader_dir,
                               IFramebuffer* /*target*/, bool color_output, bool post_process,
                               bool skybox, bool skinned) {
    name_ = name;
    shader_dir_ = shader_dir;
    color_output_ = color_output;
    post_process_ = post_process;
    skybox_ = skybox;
    skinned_ = skinned;
    debug_name_ = name;

    // ★ 不取 `vulkan_` 前缀（那是 Vulkan 专用的 450 + push constant 方言）。
    //   RenderAPI::DX12 在 resolve_one 里既不进 vulkan_ 分支，也享受
    //   engine_shaders_dir() 的核心兜底 —— 这正是我们要的 GL 版源码。
    ShaderStageSource v = resolve_shader_stage_source(name, shader_dir, ".vert", RenderAPI::DX12);
    ShaderStageSource f = resolve_shader_stage_source(name, shader_dir, ".frag", RenderAPI::DX12);
    if (!v.ok()) {
        GLOG_ERROR("D3D12Shader::load_program({}): vertex source not found (dir='{}')",
                   name, shader_dir);
        valid_ = false;
        return false;
    }
    if (!f.ok()) {
        GLOG_ERROR("D3D12Shader::load_program({}): fragment source not found (dir='{}')",
                   name, shader_dir);
        valid_ = false;
        return false;
    }
    return compile(v.code, f.code);
}

bool D3D12Shader::is_valid() const { return valid_; }

// ---------------------------------------------------------------------------
// root signature
// ---------------------------------------------------------------------------
bool D3D12Shader::create_root_signature(uint32_t srv_count, uint32_t sampler_count) {
    if (root_signature_) {
        root_signature_->Release();
        root_signature_ = nullptr;
    }

    // ★ 参数 0 / 1：两个 CBV，都在 register(b0)，靠 ShaderVisibility 区分。
    //   D3D12 的规则是"同一寄存器不允许被两个 root parameter 占用，**除非
    //   可见性不重叠**"（之前静态采样器撞车报的 E_INVALIDARG 就是这条规则，
    //   报错文本里也带着 visibility）。VERTEX 与 PIXEL 不重叠 → 合法。
    //
    //   为什么必须分开：vert 与 frag 的 uniform 集合不同，vulkanize 各自独立
    //   从偏移 0 排 std140，于是两个阶段的 `cbuffer Globals : register(b0)`
    //   **布局不同**（实测 pbr.vert 的 uModel@c0 vs pbr.frag 的 uAlbedoColor@c0）。
    //   共用一个 CBV = VS 把 frag 的第一个字段当 uModel 读 → 静默错位。
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    uint32_t param_count = 2;

    // 参数 2：SRV 描述符表（t0..t{N-1}）。
    // 可见性用 ALL：instanced.vert 会引用 readonly SSBO（StructuredBuffer、
    // t 寄存器），那是**顶点阶段**的 SRV。
    D3D12_DESCRIPTOR_RANGE srv_range{};
    if (srv_count > 0) {
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = srv_count;
        srv_range.BaseShaderRegister = 0;
        srv_range.RegisterSpace = 0;
        srv_range.OffsetInDescriptorsFromTableStart = 0;

        params[param_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[param_count].DescriptorTable.NumDescriptorRanges = 1;
        params[param_count].DescriptorTable.pDescriptorRanges = &srv_range;
        params[param_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++param_count;
    }

    // 参数 3：Sampler 描述符表（s0..s{N-1}）。
    D3D12_DESCRIPTOR_RANGE smp_range{};
    if (sampler_count > 0) {
        smp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        smp_range.NumDescriptors = sampler_count;
        smp_range.BaseShaderRegister = 0;
        smp_range.RegisterSpace = 0;
        smp_range.OffsetInDescriptorsFromTableStart = 0;

        params[param_count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[param_count].DescriptorTable.NumDescriptorRanges = 1;
        params[param_count].DescriptorTable.pDescriptorRanges = &smp_range;
        params[param_count].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        ++param_count;
    }

    // ★ 不要在这里加静态采样器！
    //   静态采样器（D3D12_STATIC_SAMPLER_DESC）会占用 s0 寄存器，而我们上面已经
    //   声明了"覆盖 s0..sN-1 的 sampler 描述符表"→ 两者寄存器范围重叠，
    //   D3D12SerializeRootSignature 直接报 E_INVALIDARG：
    //     "Shader register range of type SAMPLER (static sampler [0], visibility PIXEL)
    //      overlaps with another shader register range (root parameter[2], ... table slot [0])"
    //   实测踩过这个坑。采样器全部由描述符表提供即可 —— 引擎绑定的每个纹理都
    //   会同时写入一个 sampler 描述符（见 commit_textures）。
    //
    //   HLSL 里 `SamplerState x : register(s0)` 由 root signature 的 sampler 范围
    //   解析，描述符表完全够用。

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = param_count;
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 0;
    root_desc.pStaticSamplers = nullptr;
    // 允许输入装配器布局：引擎的 PBR/后处理 shader 都用 IA。
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    // ★ 走 loader 的函数指针，**不直接调用 D3D12SerializeRootSignature** ——
    //   后者会在链接期要求 d3d12.lib，违反"零链接期 .lib 依赖"的既定路线。
    PfnD3D12SerializeRootSignature serialize = backend_->loader().serialize_root_signature;
    if (!serialize) {
        GLOG_ERROR("D3D12Shader '{}': D3D12SerializeRootSignature unavailable (d3d12.dll too old?)",
                   debug_name_);
        return false;
    }
    HRESULT hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (FAILED(hr)) {
        const char* msg = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer())
                                   : "(no detail)";
        GLOG_ERROR("D3D12Shader '{}': D3D12SerializeRootSignature failed 0x{:08X}: {}",
                   debug_name_, static_cast<uint32_t>(hr), msg);
        if (err_blob) err_blob->Release();
        if (sig_blob) sig_blob->Release();
        return false;
    }

    hr = device_->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                      sig_blob->GetBufferSize(),
                                      __uuidof(ID3D12RootSignature),
                                      reinterpret_cast<void**>(&root_signature_));
    sig_blob->Release();
    if (err_blob) err_blob->Release();
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Shader '{}': CreateRootSignature failed 0x{:08X}", debug_name_,
                   static_cast<uint32_t>(hr));
        root_signature_ = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PSO 创建
// ---------------------------------------------------------------------------
ID3D12PipelineState* D3D12Shader::create_pso(const PsoKey& key) {
    if (!root_signature_ || vs_dxil_.empty()) return nullptr;

    // ---- 输入布局：从 key 里的顶点布局指纹还原 D3D12_INPUT_ELEMENT_DESC ----
    // key.layout.packed 的形态：[attr...] 0xFFFFFFFF <inst_stride>? [inst_attr...]?
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> semantic_names;   // 持有字符串，elements 只存指针
    semantic_names.reserve(8);

    auto append_element = [&](uint32_t location, VertexType vtype, uint32_t offset,
                              D3D12_INPUT_CLASSIFICATION cls) -> bool {
        const char* sem = nullptr;
        uint32_t sem_index = 0;
        if (!D3D12Mesh::location_to_semantic(location, &sem, &sem_index)) {
            GLOG_WARN("D3D12Shader '{}': unknown vertex location {} (skipped)", debug_name_,
                      location);
            return false;
        }
        semantic_names.emplace_back(sem);
        D3D12_INPUT_ELEMENT_DESC e{};
        e.SemanticName = nullptr;   // 稍后回填（vector 会重分配，不能存悬垂指针）
        e.SemanticIndex = sem_index;
        e.Format = dxgi_format_for_vertex_type(vtype);
        // ★ 顶点数据走 slot 0，实例数据必须走 **独立 slot 1**。
        //   D3D12 规定：**同一个 InputSlot 内的所有元素必须分类一致**
        //   （不能既有 PER_VERTEX_DATA 又有 PER_INSTANCE_DATA）。早先把实例元素
        //   也塞进 slot 0，CreateGraphicsPipelineState 直接返回 E_INVALIDARG
        //   （0x80070057）—— 因为没有任何场景用 InstancedMeshRenderer，这个 bug
        //   一直没被发现（实例化绘制在 D3D12 上是"能编译的死代码"）。
        //   对应地，绘制侧必须 IASetVertexBuffers(1, ...) 绑上实例缓冲。
        e.InputSlot = (cls == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ? 1u : 0u;
        e.AlignedByteOffset = offset;
        e.InputSlotClass = cls;
        e.InstanceDataStepRate = (cls == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ? 1 : 0;
        elements.push_back(e);
        return true;
    };

    // 顶点段
    size_t i = 0;
    for (; i + 2 < key.layout.packed.size(); i += 3) {
        if (key.layout.packed[i] == kLayoutKeySentinel) break;   // 哨兵
        append_element(key.layout.packed[i], static_cast<VertexType>(key.layout.packed[i + 1]),
                       key.layout.packed[i + 2], D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
    }
    // 实例段（哨兵之后：stride, 然后 attr 三元组）
    if (i < key.layout.packed.size() && key.layout.packed[i] == kLayoutKeySentinel) {
        ++i;   // skip sentinel
        if (i < key.layout.packed.size()) {
            ++i;   // instance stride 本身不进 INPUT_LAYOUT（只用 VBV.StrideInBytes）
            for (; i + 2 < key.layout.packed.size(); i += 3) {
                append_element(key.layout.packed[i],
                               static_cast<VertexType>(key.layout.packed[i + 1]),
                               key.layout.packed[i + 2],
                               D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA);
            }
        }
    }
    // vector 已稳定，回填指针。
    for (size_t e = 0; e < elements.size(); ++e) {
        elements[e].SemanticName = semantic_names[e].c_str();
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_;

    pso.VS.pShaderBytecode = vs_dxil_.data();
    pso.VS.BytecodeLength = vs_dxil_.size();
    pso.PS.pShaderBytecode = fs_dxil_.empty() ? nullptr : fs_dxil_.data();
    pso.PS.BytecodeLength = fs_dxil_.size();

    pso.InputLayout.pInputElementDescs = elements.empty() ? nullptr : elements.data();
    pso.InputLayout.NumElements = static_cast<UINT>(elements.size());

    // 引擎做自己的 alpha 裁剪 / two-sided 处理，用 NONE 保持与 GL 一致。
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // ---- Rasterizer ----
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = d3d12_cull_from(key.cull);
    // ★ 引擎几何约定：逆时针为正面（GL 用默认的 glFrontFace(GL_CCW)，Vulkan 显式
    //   用 VK_FRONT_FACE_COUNTER_CLOCKWISE）。D3D12 这边**不能**直接照抄成 TRUE ——
    //   **SPIRV-Cross 转 HLSL 时开了 FlipVertexY（见 d3d12_spirv_cross.cpp 的
    //   kOptionFlipVertexY），顶点 Y 一翻，三角形绕序也跟着从 CCW 变 CW**，所以这里
    //   必须填 FALSE（= 顺时针为正面）才能与引擎几何对上。
    //   写错（改成 TRUE）的症状很有欺骗性：CULL_MODE_BACK 会剔掉真正的正面、只剩
    //   内表面，几何轮廓完全正常、只是明暗/颜色不对（暗面多、色调偏移），看起来
    //   像"光照或色调映射的后端差异"，很难联想到是绕序。
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = FALSE;
    pso.RasterizerState.AntialiasedLineEnable = FALSE;
    pso.RasterizerState.ForcedSampleCount = 0;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // ---- 深度 / 模板 ----
    pso.DepthStencilState.DepthEnable = key.depth_test ? TRUE : FALSE;
    pso.DepthStencilState.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                                           : D3D12_DEPTH_WRITE_MASK_ZERO;
    // ★ 引擎约定 near→0 / far→1（与 GL 的 LESS 一致，SPIRV-Cross 已把 Z 重映射）。
    pso.DepthStencilState.DepthFunc = skybox_ ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                              : D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;

    // ---- 混合 ----
    // 数据通道（深度预通道 / G-buffer / 阴影）必须 blend=false，否则预通道写出的
    // alpha（其实是粗糙度）会与清屏色混合，污染法线 → SSR 反射方向全错。
    // 见 IShader::set_pipeline_blending 的注释。
    const bool blend_on = key.blend != 0;
    for (uint32_t i = 0; i < 8; ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[i];
        rt.BlendEnable = blend_on ? TRUE : FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;

    // ---- 渲染目标格式 ----
    // 单 RT（引擎的 MRT 只在 G-buffer 用，且 deferred 未接线；后续需要时扩）。
    pso.SampleDesc.Count = 1;
    pso.SampleDesc.Quality = 0;

    // ★ NumRenderTargets 必须与实际绑定的颜色附件数一致。
    // 阴影 / 深度 prepass 的帧缓冲是**纯深度**（没有颜色附件），此时
    // current_rt_format_ = DXGI_FORMAT_UNKNOWN。若照旧写 NumRenderTargets = 1 +
    // RTVFormats[0] = UNKNOWN，调试层报
    //   "NumRenderTargets (1) is greater than zero but all of the RTVFormat values
    //    in that range are DXGI_FORMAT_UNKNOWN"
    // 且返回 E_INVALIDARG —— 表现为"阴影 pass 完全没有 PSO，什么都不画"。
    const bool has_rt = key.rt_format != DXGI_FORMAT_UNKNOWN;
    pso.NumRenderTargets = has_rt ? 1 : 0;
    pso.RTVFormats[0] = has_rt ? static_cast<DXGI_FORMAT>(key.rt_format) : DXGI_FORMAT_UNKNOWN;
    for (UINT i = 1; i < 8; ++i) pso.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    pso.DSVFormat = static_cast<DXGI_FORMAT>(key.ds_format);

    // ★ SampleMask 默认 0 是个陷阱：0 表示"所有采样点都不写"，混合会被整个
    // 关掉（调试层会警告 Sample Mask is 0, preventing blend operations）。
    // D3D12 的 PSO desc 是零初始化的（不像 D3D11 有默认值），必须显式置全 1。
    pso.SampleMask = 0xFFFFFFFFu;

    pso.NodeMask = 0;
    pso.CachedPSO.pCachedBlob = nullptr;
    pso.CachedPSO.CachedBlobSizeInBytes = 0;
    // 允许在同一台机器上为不同适配器复编译产物（跨设备共享 PSO 库的前提）。
    pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // 无深度缓冲时 DSVFormat 必须是 UNKNOWN，否则 CreateGraphicsPipelineState
    // 返回 E_INVALIDARG（"DSVFormat must be UNKNOWN when DepthEnable is FALSE"）。
    if (pso.DSVFormat == DXGI_FORMAT_UNKNOWN) {
        pso.DepthStencilState.DepthEnable = FALSE;
    }

    ID3D12PipelineState* out = nullptr;
    HRESULT hr = device_->CreateGraphicsPipelineState(
        &pso, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&out));
    if (FAILED(hr)) {
        // 0x887A0005 = DXGI_ERROR_DEVICE_REMOVED。光看这个码排查不动，必须问设备
        // "为什么被移除" —— 返回 DEVICE_HUNG(驱动等 GPU 超时) / INVALID_CALL(非法调用)
        // / DRIVER_INTERNAL_ERROR 等，能直接把方向指向记录期还是提交期。
        const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        GLOG_ERROR("D3D12Shader '{}': CreateGraphicsPipelineState failed 0x{:08X} "
                   "(removed_reason=0x{:08X} rt={} ds={} blend={} depth={}/{} cull={} elems={})",
                   debug_name_, static_cast<uint32_t>(hr), static_cast<uint32_t>(reason),
                   key.rt_format, key.ds_format,
                   key.blend, key.depth_test, key.depth_write, key.cull, elements.size());
        return nullptr;
    }

    pso_cache_.emplace(key, out);
    return out;
}

ID3D12PipelineState* D3D12Shader::pipeline_for(const PsoKey& key) {
    auto it = pso_cache_.find(key);
    if (it != pso_cache_.end()) {
        pso_ = it->second;
        return pso_;
    }
    ID3D12PipelineState* created = create_pso(key);
    if (created) pso_ = created;
    return created;
}

// ---------------------------------------------------------------------------
// uniform 写入：name → std140 offset 表
// ---------------------------------------------------------------------------
bool D3D12Shader::split_array_index(const char* name, std::string& base, int& index) const {
    if (!name) return false;
    const std::string s(name);
    const size_t lb = s.find('[');
    if (lb == std::string::npos || s.back() != ']') return false;
    base = s.substr(0, lb);
    const std::string idx = s.substr(lb + 1, s.size() - lb - 2);
    if (idx.empty()) return false;
    for (char c : idx) {
        if (c < '0' || c > '9') return false;
    }
    index = std::atoi(idx.c_str());
    return true;
}

void D3D12Shader::write_uniform_raw(const char* name, const void* src, uint32_t size) {
    if (!name || !src) return;
    // 本帧第一次写状态 → 先把上一帧的残留清掉（见 ensure_frame 的时序说明）。
    ensure_frame();

    // ★ 同名 uniform 可能**只在一个阶段**声明（pbr.vert 的 uModel 在 frag 里没有）。
    //   所以这里往两个阶段各写一次：声明了该名字的阶段才真正落笔，另一阶段忽略。
    //   两个阶段各自的偏移不同，因此必须分别用自己的表算。
    StageUniforms* stages[2] = {&vs_u_, &ps_u_};
    for (StageUniforms* su : stages) {
        if (!su->present || su->shadow.empty()) continue;

        uint32_t offset = 0;
        auto it = su->table.find(name);
        if (it != su->table.end()) {
            offset = it->second.offset;
        } else {
            // 数组元素形式：uLightPos[3] → 基名 uLightPos + 3 * stride
            std::string base;
            int idx = 0;
            if (!split_array_index(name, base, idx)) continue;   // 该阶段没这个名字
            auto bit = su->table.find(base);
            if (bit == su->table.end()) continue;
            offset = bit->second.offset + static_cast<uint32_t>(idx) * bit->second.stride;
        }

        if (static_cast<size_t>(offset) + size > su->shadow.size()) continue;   // 越界：静默忽略
        std::memcpy(su->shadow.data() + offset, src, size);
        su->dirty = true;
    }
}

void D3D12Shader::set_int(const std::string& name, int value) {
    // 本帧第一次写状态 → 先复位（这条分支可能不经过 write_uniform_raw，
    // 因为采样器名会被拦截在上面，所以这里也要触发一次）。
    ensure_frame();
    // ★ 采样器拦截：引擎用 `set_int(<GLSL采样器名>, <引擎槽位>)` 告知"某个槽位上
    //   是哪个采样器"（见 render_context.cpp:86、material.cpp:308）。
    //   对 GL 而言这个 int 是纹理单元号（要写进 uniform）；对 D3D12 而言它是
    //   **槽位号**，是纹理绑定解析的关键信息 —— 必须记下来，绝不能写进 CBV
    //   （CBV 里根本没有这个变量）。
    //
    //   注意 `value` 就是引擎槽位，因为 IRenderBackend::texture_unit_for_slot 的
    //   默认实现是恒等的（render.h:86），D3D12 不覆写它。
    if (sampler_index_for_name(name) >= 0) {
        slot_sampler_name_[value] = name;   // slot → 采样器名（本帧后续空名绑定要用）
        pending_sampler_name_ = name;       // 紧邻的 set_texture 可当场精确落位
        return;
    }
    write_uniform_raw(name.c_str(), &value, sizeof(int));
}
void D3D12Shader::set_int(const char* name, int value) {
    if (!name) return;
    set_int(std::string(name), value);   // 复用上面的采样器拦截
}
void D3D12Shader::set_float(const std::string& name, float value) {
    write_uniform_raw(name.c_str(), &value, sizeof(float));
}
void D3D12Shader::set_float(const char* name, float value) {
    write_uniform_raw(name, &value, sizeof(float));
}
void D3D12Shader::set_vec2(const std::string& name, const math::Vector2f& value) {
    write_uniform_raw(name.c_str(), &value, sizeof(float) * 2);
}
void D3D12Shader::set_vec2(const char* name, const math::Vector2f& value) {
    write_uniform_raw(name, &value, sizeof(float) * 2);
}
void D3D12Shader::set_vec3(const std::string& name, const math::Vector3f& value) {
    // std140 的 vec3 占 16 字节（末尾 4 字节 padding）—— 只写 12 字节，
    // 剩下 4 字节保持 0 是正确行为（HLSL 的 float3 也不读第 4 个分量）。
    write_uniform_raw(name.c_str(), &value, sizeof(float) * 3);
}
void D3D12Shader::set_vec3(const char* name, const math::Vector3f& value) {
    write_uniform_raw(name, &value, sizeof(float) * 3);
}
void D3D12Shader::set_vec4(const std::string& name, const math::Vector4f& value) {
    write_uniform_raw(name.c_str(), &value, sizeof(float) * 4);
}
void D3D12Shader::set_vec4(const char* name, const math::Vector4f& value) {
    write_uniform_raw(name, &value, sizeof(float) * 4);
}
void D3D12Shader::set_mat4(const std::string& name, const math::Matrix4f& value) {
    write_uniform_raw(name.c_str(), &value, sizeof(float) * 16);
}
void D3D12Shader::set_mat4(const char* name, const math::Matrix4f& value) {
    write_uniform_raw(name, &value, sizeof(float) * 16);
}

void D3D12Shader::set_mat4_array(const char* name, const math::Matrix4f* data, uint32_t count) {
    if (!name || !data || count == 0) return;

    StageUniforms* stages[2] = {&vs_u_, &ps_u_};
    for (StageUniforms* su : stages) {
        if (!su->present || su->shadow.empty()) continue;
        auto it = su->table.find(name);
        if (it == su->table.end()) continue;
        const UniformEntry& e = it->second;
        const uint32_t max = (e.array_count > 0) ? e.array_count : count;
        const uint32_t n = (count < max) ? count : max;
        if (n < count) {
            GLOG_WARN("D3D12Shader '{}': set_mat4_array('{}') truncated {} -> {}", debug_name_,
                      name, count, n);
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t off = e.offset + i * e.stride;
            if (static_cast<size_t>(off) + sizeof(float) * 16 > su->shadow.size()) break;
            std::memcpy(su->shadow.data() + off, &data[i], sizeof(float) * 16);
        }
        su->dirty = true;
    }
}

void D3D12Shader::set_pipeline_blending(bool enabled) {
    ensure_frame();
    blending_ = enabled;
}

// ---------------------------------------------------------------------------
// set_post_process_params —— 与 GL 版逐名对照（gl_shader.cpp:609）
// ---------------------------------------------------------------------------
// 不做任何"延迟应用"（GL 因为 uniform location 查询昂贵才攒 pp_dirty_）：
// 直接按名字写进本帧 uniform 影子内存。名字不在某阶段的表里时
// write_uniform_raw 会静默跳过，所以三张表可以无脑全量下发。
//
// ★ 这张名单必须与 gl_shader.cpp 的 floats/ints/vec4s 三表**严格一致**。
//   漏一项的后果就是那个特效悄悄失效（如 uContactShadowEnabled 恒 0 →
//   接触阴影整个没接上；uSSRNear 恒 0 → SSR 除零 → TDR）。
// ---------------------------------------------------------------------------
void D3D12Shader::set_post_process_params(const PostProcessParams& params) {
    ensure_frame();   // 本帧第一次写状态 → 先复位（与其它 set_* 一致）

    const PostProcessParams& p = params;

    struct Uniform1f { const char* name; float value; };
    const Uniform1f floats[] = {
        {"uExposure", p.exposure},
        {"uEV100", p.ev100},
        {"uWhitePoint", p.white_point},
        {"uBlackPoint", p.black_point},
        {"uContrast", p.contrast},
        {"uSaturation", p.saturation},
        {"uBloomThreshold", p.bloom_threshold},
        {"uBloomIntensity", p.bloom_intensity},
        {"uFilmGrain", p.film_grain},
        {"uVignette", p.vignette},
        {"uChromaticAberration", p.chromatic_aberration},
        {"uLUTStrength", p.lut_strength},
        {"uAETargetLuminance", p.ae_target_luminance},
        {"uAEMinExposure", p.ae_min_exposure},
        {"uAEMaxExposure", p.ae_max_exposure},
        {"uAESpeed", p.ae_speed},
        {"uTAAWeight", p.taa_weight},
        {"uMotionBlurAmount", p.motion_blur_amount},
        {"uSSAOStrength", p.ssao_strength},
        {"uSSAORadius", p.ssao_radius},
        {"uSSAONear", p.ssao_near},
        {"uSSAOFar", p.ssao_far},
        {"uSSAOTanHalfFov", p.ssao_tan_half},
        {"uSSAOAspect", p.ssao_aspect},
        {"uSSRMaxRoughness", p.ssr_max_roughness},
        {"uSSRThickness", p.ssr_thickness},
        {"uSSRBilateralFilter", p.ssr_bilateral_filter},
        {"uSSREnvFallback", p.ssr_env_fallback},
        {"uSSRDebugMode", static_cast<float>(p.ssr_debug_view)},
        {"uSSRNear", p.ssr_near},
        {"uSSRFar", p.ssr_far},
        {"uSSRTanHalfFov", p.ssr_tan_half},
        {"uSSRAspect", p.ssr_aspect},
        {"uContactShadowStrength", p.cs_strength},
    };
    for (const auto& u : floats) {
        write_uniform_raw(u.name, &u.value, sizeof(float));
    }

    struct Uniform1i { const char* name; int value; };
    const Uniform1i ints[] = {
        {"uToneMapMode", p.tone_map_mode},
        {"uDithering", p.dithering},
        {"uBloomEnabled", p.bloom_enabled},
        {"uUseLUT", p.use_lut},
        {"uAutoExposure", p.auto_exposure},
        {"uTAAEnabled", p.taa_enabled},
        {"uSSAOEnabled", p.ssao_enabled},
        {"uSSREnabled", p.ssr_enabled},
        {"uSSRMaxSteps", p.ssr_max_steps},
        {"uMotionBlurEnabled", p.motion_blur_enabled},
        {"uContactShadowEnabled", p.cs_enabled},
    };
    for (const auto& u : ints) {
        write_uniform_raw(u.name, &u.value, sizeof(int));
    }

    struct Uniform4f { const char* name; math::Vector4f value; };
    const Uniform4f vec4s[] = {
        {"uLift", p.lift},
        {"uGamma", p.gamma},
        {"uGain", p.gain},
        {"uShadows", p.shadows},
        {"uMidtones", p.midtones},
        {"uHighlights", p.highlights},
    };
    for (const auto& u : vec4s) {
        const float v[4] = {u.value.x, u.value.y, u.value.z, u.value.w};
        write_uniform_raw(u.name, v, sizeof(float) * 4);
    }
}

// ---------------------------------------------------------------------------
// 纹理绑定
// ---------------------------------------------------------------------------
int D3D12Shader::sampler_index_for_name(const std::string& name) const {
    for (size_t i = 0; i < sampler_names_.size(); ++i) {
        if (sampler_names_[i] == name) return static_cast<int>(i);
    }
    return -1;
}

void D3D12Shader::bind_texture_at_index(int index, D3D12Texture* tex) {
    if (index < 0 || index >= static_cast<int>(bound_textures_.size())) return;
    bound_textures_[index].tex = tex;
    textures_dirty_ = true;
}

void D3D12Shader::set_texture(int slot, ITexture* texture) {
    // ★★ 这里是 D3D12 与 GL/Vulkan 最大的语义差异，也是"画面全黑"的根因。★★
    //
    // 引擎的绑定调用点（material.cpp:307 / render_pipeline.cpp 数十处）几乎都长这样：
    //     ctx->set_texture(shader, tex, slot, "");          // ← 名字是空串！
    //     ctx->set_uniform_int(shader, "uNormalMap", slot); // ← 名字在这里
    // 或
    //     ctx->set_texture(shader, tex, slot, "uSSAOTexture");
    //     ctx->set_uniform_int(shader, "uSSAOTexture", slot);
    //
    // 也就是说 **slot 才是可靠的键**，名字只是一个"顺带告知"的信号，而且还可能
    // 出现在 set_texture 之前（SetTexture 命令的处理器）或之后（Material::bind）。
    //
    // 因此 D3D12 侧不能"靠紧邻的前一个 set_int 认名字"（早先的实现就是这样，
    // 于是绝大多数绑定因为 pending_sampler_name_ 为空被整个丢掉 → 全部 SRV 落成
    // 空描述符 → 每个 pass 都输出 0 → 整屏纯黑）。
    //
    // 正确流程：
    //   1) 命名绑定：set_int(name, slot) 紧邻在前 → 当场精确落位；
    //   2) 空名绑定：按 slot 记进 pending_by_slot_，等 commit_textures() 解析。
    // ★ 本帧第一次写状态 → 先复位（否则会把本帧已记下的绑定整块擦掉）。
    ensure_frame();
    auto* t = dynamic_cast<D3D12Texture*>(texture);

    if (pending_sampler_name_.has_value()) {
        const std::string name = *pending_sampler_name_;
        pending_sampler_name_.reset();
        slot_sampler_name_[slot] = name;   // 顺手记牢 slot↔name，供后续空名绑定复用
        pending_by_slot_.erase(slot);      // 精确绑定覆盖同 slot 的延迟项
        const int index = sampler_index_for_name(name);
        if (index >= 0 && index < static_cast<int>(bound_textures_.size())) {
            bound_textures_[index].tex = t;
            textures_dirty_ = true;
            return;
        }
        // 该名字不在本 shader 的采样器表里（例如把别的 pass 的名字传了过来）。
        // 不丢弃：退回按 slot 延迟解析。
    }

    // 空名绑定：只记"哪个 slot 要绑哪张图"，名字稍后补齐。
    pending_by_slot_[slot] = t;
    textures_dirty_ = true;
}

// 把 pending_by_slot_ 里攒下的"slot → 纹理"，用已学到的 slot→采样器名映射
// 解析成 bound_textures_[声明序] 上的真实绑定。
// 必须在 commit_textures()（真正建描述符表）之前调用。
void D3D12Shader::resolve_pending_textures() {
    if (pending_by_slot_.empty()) return;

    for (const auto& [slot, tex] : pending_by_slot_) {
        auto nit = slot_sampler_name_.find(slot);
        if (nit == slot_sampler_name_.end()) {
            // 从未见过这个 slot 对应的采样器名：引擎只做了 bind(unit) + 空名
            // set_texture，且没有 set_uniform_int 告知名字。
            GLOG_WARN("D3D12Shader '{}': texture bind on slot {} cannot be resolved to a "
                      "sampler name (no set_uniform_int seen for this slot); texture dropped",
                      debug_name_, slot);
            continue;
        }
        const int index = sampler_index_for_name(nit->second);
        if (index < 0 || index >= static_cast<int>(bound_textures_.size())) {
            GLOG_WARN("D3D12Shader '{}': slot {} → sampler '{}' is not declared in this shader",
                      debug_name_, slot, nit->second);
            continue;
        }
        bound_textures_[index].tex = tex;
    }
    pending_by_slot_.clear();
}

// ---------------------------------------------------------------------------
// 外部 SSBO 的 SRV 绑定（GPU 剔除的实例流）
// ---------------------------------------------------------------------------
void D3D12Shader::set_external_buffer_srv(const char* block_name, ID3D12Resource* res,
                                          uint64_t size_bytes) {
    if (!block_name || !*block_name || !res) return;
    external_srv_[block_name] = ExternalSrv{res, size_bytes};
}

// 把 external_srv_ 里登记的 SSBO 写成 StructuredBuffer SRV，放到表内该块的 t 寄存器槽位。
// 必须在"表已建好（含 null 填充与纹理 SRV）之后"调用 —— 否则会被覆盖成空描述符。
void D3D12Shader::apply_external_buffer_srvs(uint32_t srv_base) {
    if (!backend_ || !backend_->device() || external_srv_.empty()) return;
    if (srv_register_count_ == 0) return;

    DescriptorHeapSet& heaps = backend_->descriptor_heaps();
    for (const auto& [name, ext] : external_srv_) {
        const SsboBinding* sb = find_ssbo(ssbo_bindings_, name);
        if (!sb) {
            // 只警告一次（每 draw 都调，刷屏会把真正的错误淹掉）。
            static thread_local std::set<std::string> warned;
            if (warned.insert(debug_name_ + "/" + name).second) {
                GLOG_WARN("D3D12Shader '{}': external SSBO block '{}' not found in the compiled "
                          "shader (buffer SRV not bound)", debug_name_, name);
            }
            continue;
        }
        if (sb->is_uav && sb->reg >= srv_register_count_) {
            continue;
        }
        if (sb->reg >= srv_register_count_) continue;

        const uint32_t stride = sb->stride ? sb->stride : 4u;
        // raw（ByteAddressBuffer）必须用 R32_TYPELESS + stride 0 + RAW 标志；
        // structured 用 UNKNOWN + stride + FLAG_NONE。混用会让设备被移除。
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = sb->is_raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = 0;
        d.Buffer.NumElements = static_cast<uint32_t>(ext.size / (sb->is_raw ? 4u : stride));
        d.Buffer.StructureByteStride = sb->is_raw ? 0u : stride;
        d.Buffer.Flags = sb->is_raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        backend_->device()->CreateShaderResourceView(ext.res, &d,
                                                     heaps.srv().cpu_handle(srv_base + sb->reg));
    }
}

bool D3D12Shader::commit_textures() {
    if (!backend_) return true;
    if (srv_register_count_ == 0 && sampler_register_count_ == 0) return true;

    // ★ 先把本帧按 slot 攒下的空名绑定解析成真实的 bound_textures_ 落位。
    //   必须在这里做（而不是在 set_texture 里当场做）：引擎经常先 set_texture
    //   再 set_uniform_int(<名字>, <slot>)（material.cpp:307-308），名字直到
    //   后一步才出现；只有到 commit（= draw 前一刻）才一定齐了。
    resolve_pending_textures();

    // 本帧已建过表，且当前纹理绑定与建表时**完全一致** → 直接复用。
    // 引擎几乎每个 draw 都会重新 set_texture（即使值没变），若不比较就会
    // 每 draw 重新申请一段描述符 → 堆迅速耗尽并反复扩容。
    if (!srv_runs_.empty() && !textures_dirty_) {
        // 表没变，但外部 SSBO 绑定（实例流）可能在本帧才登记 → 仍要补写槽位。
        apply_external_buffer_srvs(current_srv_base_);
        return true;
    }
    if (!srv_runs_.empty() && bound_signature_.size() == bound_textures_.size()) {
        bool same = true;
        for (size_t i = 0; i < bound_textures_.size(); ++i) {
            if (bound_signature_[i] != bound_textures_[i].tex) { same = false; break; }
        }
        if (same) {
            textures_dirty_ = false;
            // 同上：复用表也要补 shader 可读屏障（资源状态可能已被别的 pass 改过）。
            for (const auto& bt : bound_textures_) {
                if (bt.tex && bt.tex->is_valid()) {
                    backend_->ensure_texture_shader_readable(bt.tex);
                }
            }
            apply_external_buffer_srvs(current_srv_base_);
            return true;
        }
    }
    textures_dirty_ = false;

    DescriptorHeapSet& heaps = backend_->descriptor_heaps();

    // ★ 按"纹理集合指纹"缓存本帧已建好的描述符区间。
    //
    // 上一版只在**连续两次**绑定相同时才复用（bound_signature_ 比较），
    // 于是"绘制顺序  A B A C A"  会为 A 分配 3 次。pbr 一次要吃 26 个 sampler
    // 槽位，十几二十个 draw 就把 sampler 堆（D3D12 硬上限 2048）撑爆 → 触发
    // 运行期扩容 → 已录制的表句柄全部悬空 → 调试层崩在 d3d12SDKLayers.dll /
    // 关掉调试层时 RemoveDevice。
    // 改成"整帧内按指纹复用"，用量从 O(draw 数) 降到 O(不同纹理集合数)。
    const uint64_t sig = texture_set_fingerprint();
    if (sig != 0) {
        auto it = run_cache_.find(sig);
        if (it != run_cache_.end()) {
            const DescriptorRun& r = it->second;
            srv_table_gpu_ = heaps.srv().gpu_handle(r.base);
            sampler_table_gpu_ = heaps.sampler().gpu_handle(r.smp_base);
            current_srv_base_ = r.base;
            // ★ 复用描述符表时**仍然**要确保每张纹理处于 shader 可读态。
            //   缓存里存的是"表的槽位"，不是"资源的当前状态"——同一张图可能
            //   在上一个 pass 里被当作 RT 写过（effect 的 ping-pong 目标就是
            //   这种用法），状态已经变回 RENDER_TARGET。跳过这一步会让 D3D12
            //   采样 RT 态资源 → Intel Arc 上直接 DEVICE_HUNG。
            for (const auto& bt : bound_textures_) {
                if (bt.tex && bt.tex->is_valid()) {
                    backend_->ensure_texture_shader_readable(bt.tex);
                }
            }
            apply_external_buffer_srvs(r.base);
            return true;
        }
    }

    // ★ 描述符表必须是一段**连续**区间，且表内第 k 个槽位对应寄存器 t{k} / s{k}。
    //
    //   关键对齐问题（实测）：vulkanize 把 Globals 块放在 `binding = 0`，
    //   采样器从 `binding = 1` 起顺排；SPIRV-Cross **原样保留 binding 号**作为
    //   HLSL 寄存器号 → 采样器实际落在 t1..tN / s1..sN，**t0 / s0 是空的**。
    //   所以不能"从表首顺排填写" —— 那样第一个纹理会被放到 t0，
    //   而 HLSL 期望它在 t1 → 纹理全错位。
    //   正确做法：一次申请覆盖 t0..t{max} 的整段，把第 i 个纹理写到槽位 (1+i)。
    //
    //   ★ 范围必须 ≥ sampler 数 + 1：若某个采样器被编译器消掉（HLSL 里不再出现
    //     它的 register(tN)），扫出来的 max 会小于采样器个数，而绑定循环仍按
    //     采样器个数写槽位 → 越界写描述符（越出堆 → 访问违规）。这里显式取 max。
    const uint32_t need = static_cast<uint32_t>(sampler_names_.size()) + 1u;
    const uint32_t srv_count = srv_register_count_ > need ? srv_register_count_ : need;
    const uint32_t smp_count = sampler_register_count_ > need ? sampler_register_count_ : need;

    uint32_t srv_base = 0, smp_base = 0;
    if (!heaps.srv().allocate_range(srv_count, srv_base)) {
        GLOG_ERROR("D3D12Shader '{}': SRV heap exhausted ({} slots)", debug_name_, srv_count);
        return false;
    }
    if (!heaps.sampler().allocate_range(smp_count, smp_base)) {
        heaps.srv().free_range(srv_base, srv_count);
        GLOG_ERROR("D3D12Shader '{}': Sampler heap exhausted ({} slots)", debug_name_, smp_count);
        return false;
    }
    srv_runs_.push_back({srv_base, srv_count});
    sampler_runs_.push_back({smp_base, smp_count});

    // ---- 先给整段写"空描述符" ----
    // D3D12 要求被绑定的描述符表里每个槽位都有有效内容；SSBO 等本引擎尚未
    // 接线的资源会落在这些空槽上（读了会得到 0，而不是未定义/触发校验）。
    //
    // ★ 空描述符（pResource = nullptr）**不能**把 desc.Format 写成
    // DXGI_FORMAT_UNKNOWN —— 调试层会连报
    //   "The Format (0, UNKNOWN) is invalid ... the Resource does not have a fully
    //    typed format"，紧接着 RemoveDevice(DXGI_ERROR_INVALID_CALL)，设备永久失效。
    //   也不能 pDesc = nullptr（"cannot be created from a NULL Resource and NULL Desc"）。
    // 正确写法是 **pResource = nullptr + 一个格式合法的 desc**：这才是 D3D12 的
    // "null descriptor"惯例，采样返回 0 且不触发任何校验。
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Texture2D.MostDetailedMip = 0;
    null_srv.Texture2D.MipLevels = 1;
    null_srv.Texture2D.PlaneSlice = 0;
    null_srv.Texture2D.ResourceMinLODClamp = 0.0f;
    for (uint32_t i = 0; i < srv_count; ++i) {
        backend_->device()->CreateShaderResourceView(nullptr, &null_srv,
                                                     heaps.srv().cpu_handle(srv_base + i));
    }

    // 默认采样器：非比较过滤。
    // ★ ComparisonFunc 必须与 Filter 匹配，否则调试层对**每一次** CreateSampler
    //   都报一条 WARNING；本引擎每个 draw 都会重建整段采样器描述符，几百条警告
    //   会把 ID3D12InfoQueue 的 1024 条环形缓冲**冲爆**，真正的错误被挤出去
    //   （排查时吃过这个亏）。非比较过滤 → 用 NEVER 才是"意图明确"的写法。
    D3D12_SAMPLER_DESC def_smp{};
    def_smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    def_smp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    def_smp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    def_smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    def_smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    def_smp.MaxAnisotropy = 1;
    def_smp.MaxLOD = D3D12_FLOAT32_MAX;
    for (uint32_t i = 0; i < smp_count; ++i) {
        backend_->device()->CreateSampler(&def_smp, heaps.sampler().cpu_handle(smp_base + i));
    }

    // ---- 真正绑定的纹理写到槽位 (1 + i) ----
    const size_t n = sampler_names_.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t srv_slot = srv_base + 1 + static_cast<uint32_t>(i);
        const uint32_t smp_slot = smp_base + 1 + static_cast<uint32_t>(i);
        bound_textures_[i].srv_slot = srv_slot;
        bound_textures_[i].smp_slot = smp_slot;
        bound_textures_[i].allocated = true;

        D3D12Texture* tex = bound_textures_[i].tex;
        if (!tex || !tex->is_valid()) continue;   // 未绑定 → 保留空描述符

        // ★ 采样前必须把资源转到 "shader 可读"：effect 的离屏目标（SSR blur/
        //   composite、bloom 各级）刚被上一个 pass 当 RT 写过，当前状态是
        //   RENDER_TARGET。D3D12 采样这种资源是非法用法 —— 调试层可能只给一条
        //   静默警告，Intel Arc 上直接 GPU 挂起（DEVICE_HUNG）。Vulkan 由 render
        //   pass 的 finalLayout 自动处理，D3D12 必须在这里显式补屏障。
        backend_->ensure_texture_shader_readable(tex);

        tex->create_srv(heaps.srv().cpu_handle(srv_slot));

        D3D12_SAMPLER_DESC sd{};
        sd.Filter = tex->dxgi_filter();
        sd.AddressU = tex->dxgi_address_u();
        sd.AddressV = tex->dxgi_address_v();
        sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MipLODBias = 0.0f;
        sd.MaxAnisotropy = 1;
        // ★ ComparisonFunc 只在比较过滤（阴影采样）下有意义。D3D12 的调试层对
        //   "非比较过滤 + ComparisonFunc != NEVER" 每一次 CreateSampler 都报一条
        //   WARNING —— 本引擎每个 draw 都会重建采样器描述符，几百条警告会把
        //   ID3D12InfoQueue 的 1024 条环形缓冲**冲爆**，把真正的错误挤出去
        //   （排查时吃过这个亏）。所以按过滤类型给合规的值。
        const bool comparison_filter =
            (static_cast<uint32_t>(sd.Filter) & 0x80u) != 0u;   // D3D12_FILTER 的比较位
        sd.ComparisonFunc = comparison_filter ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                              : D3D12_COMPARISON_FUNC_NEVER;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.0f;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        backend_->device()->CreateSampler(&sd, heaps.sampler().cpu_handle(smp_slot));
    }

    // 表句柄 = 该段的首地址（槽位 0 ↔ 寄存器 t0 / s0）。
    srv_table_gpu_ = heaps.srv().gpu_handle(srv_base);
    sampler_table_gpu_ = heaps.sampler().gpu_handle(smp_base);

    // ★★ 本函数一共四条出口，**每一条**都必须做这两件事：把外部 SSBO 的 SRV
    //    写进刚建好的这段表，并把 current_srv_base_ 更新成本段的基址。
    //
    //    漏掉的后果极具迷惑性（2026-10-10 实测，排查花了很久）：上面那个
    //    "整段写 null 描述符"的循环会先把 DstModels（instanced.vert 的
    //    可见实例矩阵 SSBO）所在槽位写成空描述符，新建表这条出口若不补写，
    //    顶点着色器读实例矩阵就**恒为 0** → 所有实例退化到原点 → GPU 剔除
    //    路径"一个实例都看不见"。而算错的只有这一个描述符：间接参数、
    //    剔除结果、dst_models 的内容**全都是对的**（readback 逐项验过），
    //    所以从日志/数值上完全看不出问题。
    //    另一条出口用 current_srv_base_ 复用本段，若这里没更新它，
    //    后续所有 draw 都会把 SSBO 描述符写到**别的表**里，同样静默失效。
    current_srv_base_ = srv_base;
    apply_external_buffer_srvs(srv_base);

    // 记进指纹缓存：本帧后续任何 draw 只要纹理集合相同就直接复用这段，
    // 不再重复申请描述符（见上面的说明）。
    if (sig != 0) {
        run_cache_.emplace(sig, DescriptorRun{ srv_base, srv_count, smp_base, smp_count });
    }

    // 记下本次建表时的纹理集合，供"绑定没变就复用"比较。
    bound_signature_.resize(bound_textures_.size());
    for (size_t i = 0; i < bound_textures_.size(); ++i) bound_signature_[i] = bound_textures_[i].tex;
    return true;
}

// 把当前纹理绑定的**指针数组**哈希成一个指纹。
// 0 保留为"不可用"（空集合也返回非 0，见下：混入一个常量种子）。
uint64_t D3D12Shader::texture_set_fingerprint() const {
    uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
    for (const auto& b : bound_textures_) {
        const uint64_t p = reinterpret_cast<uint64_t>(b.tex);
        h ^= p;
        h *= 1099511628211ull;             // FNV prime
    }
    return h == 0 ? 1ull : h;
}

// ---------------------------------------------------------------------------
// 提交 uniform → GPU
// ---------------------------------------------------------------------------
D3D12Shader::UniformCbv D3D12Shader::commit_uniforms() {
    UniformCbv out;
    if (!backend_) return out;

    StageUniforms* stages[2] = {&vs_u_, &ps_u_};
    D3D12_GPU_VIRTUAL_ADDRESS* slots[2] = {&out.vs, &out.ps};

    for (int i = 0; i < 2; ++i) {
        StageUniforms& su = *stages[i];
        if (!su.present || su.block_size == 0 || su.shadow.empty()) continue;

        // ★ 每次 draw 都申请新的一段：同一 shader 一帧内被画多次、每次值不同，
        //   共用一段会被后面的 draw 覆盖（与 Vulkan 每 draw 独立描述符集同理）。
        //   UploadRing::allocate_and_copy 内部按 256（kConstantBufferAlignment）
        //   对齐 cursor 并返回该段的 GPU 地址 —— 正好是 CBV 需要的形态。
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        if (!backend_->upload_ring().allocate_and_copy(su.shadow.data(), su.block_size, gpu)) {
            GLOG_WARN("D3D12Shader '{}': upload ring exhausted ({} bytes needed)", debug_name_,
                      su.block_size);
            continue;
        }
        su.gpu = gpu;
        su.dirty = false;
        *slots[i] = gpu;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 每帧重置：ensure_frame / on_begin_frame
// ---------------------------------------------------------------------------
// ★★ 这里是整个 D3D12 后端最容易踩、也最难查的时序坑 ★★
//
// RenderContext 是**命令缓冲**，一帧的实际调用顺序是：
//     D3D12Backend::begin_frame()          ← ++frame_counter_
//     dispatch: SetShader / SetUniform* / SetTexture …   ← shader 状态在这里累积
//     dispatch: DrawMesh → D3D12Backend::draw_mesh()
//     D3D12Backend::end_frame()
//
// 即 **"设置状态" 严格早于 "draw"**。所以"每帧重置"（复位 uniform 影子内存、
// 清纹理绑定、归还描述符区间）只能在 **本帧第一次写 shader 状态之前** 发生。
//
// 早先的实现把重置放在 draw_mesh 里（`s->on_begin_frame(frame_counter_)`），
// 于是每一条 draw 都会把"本帧刚设好的 uniform 与纹理绑定"整块擦掉：
//     * 所有 SRV 落到空描述符 → 采样恒定返回 0；
//     * uniform 回到 GLSL 默认值；
// 表现就是**整帧纯黑**，但同时深度写入/清屏完全正常（它们不依赖采样），
// 极容易被误判成"几何没画进去"或"回读坏了"。
//
// 修法：把触发点改成"第一次写状态"（ensure_frame），draw 路径**不再**触发。
// 判定用后端帧号，故幂等；`bind()` 也触发一次（SetShader 通常是本 draw 的第一条）。
// ---------------------------------------------------------------------------
void D3D12Shader::ensure_frame() {
    if (!backend_) return;
    const uint64_t fid = backend_->current_frame_id();
    if (fid == last_frame_id_) return;   // 本帧已复位过
    on_begin_frame(fid);
}

void D3D12Shader::on_begin_frame(uint64_t frame_id) {
    last_frame_id_ = frame_id;

    // 每个阶段：影子内存复位成"零 + 已求值的 GLSL 初始化式"。
    // ★ 不直接清零：GL 的 uniform 是**跨帧保持**的，且 tonemap 的
    //   uExposure/uGamma/uSaturation 等默认值非 0（GLSL 里写了 `= 1.0`）。
    //   清零会让画面全黑/全灰。用默认快照复位 = 与 GL 语义一致。
    for (StageUniforms* su : {&vs_u_, &ps_u_}) {
        if (!su->present) continue;
        su->shadow = su->defaults;
        su->dirty = false;
        su->gpu = 0;
    }
    for (auto& bt : bound_textures_) bt.tex = nullptr;
    textures_dirty_ = false;
    // 外部 SSBO（GPU 剔除的实例流）是"每 draw 登记"的绑定意图，随帧作废。
    external_srv_.clear();
    pending_sampler_name_.reset();
    // 延迟绑定是"本帧的绑定意图"，随帧作废。
    // slot_sampler_name_ **不**清：槽位↔采样器名是引擎的固定约定（TextureSlots），
    // 跨帧沿用能让"某一帧恰好没发 set_uniform_int"的情况也不会丢掉纹理。
    pending_by_slot_.clear();
    pso_ = nullptr;

    // 描述符槽位归还（★ 必须延迟到 GPU 真正跑过引用它们的帧）。
    //
    // 这里**不能**直接 free_range：调用点是"本帧第一次用这个 shader 时"，
    // 而引擎有 frames_in_flight=3 个帧槽 —— 上帧那批描述符此刻多半仍被
    // "已提交但未完成"的命令列表引用。立即归还 → 区间马上被分配给别的纹理
    // → GPU 读到被改写/空的描述符 → Intel Arc 上 DXGI_ERROR_DEVICE_HUNG。
    // 交给 backend 按围栏值排队，drain_retired() 在 GPU 跑过后才真正 free。
    if (backend_) {
        for (const DescriptorRun& r : srv_runs_) {
            backend_->retire_descriptor_range(r.base, r.count, 0, 0);
        }
        for (const DescriptorRun& r : sampler_runs_) {
            backend_->retire_descriptor_range(0, 0, r.base, r.count);
        }
        srv_runs_.clear();
        sampler_runs_.clear();
        // 指纹缓存与描述符区间同生命周期：区间已（排队）归还，缓存必须一起作废
        // （否则下一帧会拿回指向已释放槽位的表句柄）。
        run_cache_.clear();
        bound_signature_.clear();
        for (auto& bt : bound_textures_) bt.allocated = false;
    }
}

// ---------------------------------------------------------------------------
// bind / unbind —— D3D12 的绑定发生在 draw 路径（需要 command list），
// 这里保持空实现，与 VulkanShader 一致。
// ---------------------------------------------------------------------------
void D3D12Shader::bind() const {
    // SetShader 通常是本 draw 的**第一条**命令（引擎的 draw 辅助函数都是
    // set_shader → set_uniform_* / set_texture → draw_*），所以这里触发复位最稳，
    // 能覆盖"某帧一条 uniform/纹理都没设就直接 draw"的退化情形。
    // D3D12 的绑定发生在 draw 路径，bind() 本身无别的动作（同 VulkanShader），
    // 复位状态属于"缓存的每帧状态"，逻辑上不影响 const 语义 → const_cast 是刻意的。
    const_cast<D3D12Shader*>(this)->ensure_frame();
}
void D3D12Shader::unbind() const {}

} // namespace gryce_engine::render::d3d12
