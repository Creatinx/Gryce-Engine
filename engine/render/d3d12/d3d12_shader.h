// =============================================================================
// d3d12_shader — IShader 的 D3D12 实现（M4.4）
//
// 这是整个 D3D12 后端最关键的一层：把"引擎的 IShader 约定"翻译成
// "root signature + PSO + 每帧常量缓冲 + 每 draw 描述符表"。
//
// ---------------------------------------------------------------------------
// 一、与 GL / Vulkan 的根本差异：uniform 不再是"按名字设置"
// ---------------------------------------------------------------------------
// GL 有 glGetUniformLocation(program, name)，Vulkan 侧 vk_shader.cpp 干脆手写了
// 一个 packed UBO 结构体（ubo_data_）在 C++ 端逐字段赋值。
//
// D3D12 走**第三条路**，也是唯一自洽的路：D3D12 的常量缓冲是**裸内存块 + 视图**，
// root signature 只声明"哪个寄存器上放一个 CBV"，没有任何反射能力。所以：
//
//   * CBV 的**字节布局**由 M4.1 的 vulkanize 阶段算出（std140 → HLSL packoffset，
//     SPIRV-Cross 已保证两者偏移完全一致），我们直接复用 uniform_fields 的
//     (name → offset, size) 表；
//   * `set_float("uRoughness", 0.5f)` 的动作 = 找到 uRoughness 的 offset，
//     往本帧 CBV 的 CPU 影子内存里 memcpy；
//   * 提交 draw 前把影子内存整块拷进 UploadRing 的一段，用它的 GPU 地址建 CBV。
//
// 这个设计的好处：**不需要为每个着色器手写字段**。pbr.frag 有 81 个 uniform、
// scene_pbr.frag 更多，手写字段是维护灾难；而 std140 偏移是自动算出来的，
// 新增 uniform 无需改 C++。
//
// ---------------------------------------------------------------------------
// 二、root signature 布局
// ---------------------------------------------------------------------------
//   * register(b0)  : CBV —— Globals 常量块，**VS 一个、PS 一个**（见下）
//   * register(t0..): SRV 描述符表，覆盖 HLSL 里出现的最大 t 寄存器
//   * register(s0..): Sampler 描述符表，覆盖 HLSL 里出现的最大 s 寄存器
//
// ★ 为什么 CBV 要分两个（实测结论，不是设计偏好）
// ---------------------------------------------------------------------------
// vert 与 frag 的 uniform 集合**不同**，vulkanize 各自独立按 std140 从偏移 0
// 开始排布，于是两个阶段各自产出 `cbuffer Globals : register(b0)`，但**偏移
// 完全不同**。实测 pbr 的 SPIRV-Cross 输出：
//
//     pbr.vert : _13_uModel @c0, _13_uView @c4, _13_uProjection @c8
//     pbr.frag : _431_uAlbedoColor @c0, _431_uRoughness @c0.w, ...
//
// 两者共用一个 CBV 的后果：VS 会把 frag 的 uAlbedoColor 当成 uModel 读 →
// **画面静默错位**（不报错、不崩，最难查的一类）。
//
// D3D12 的规则是"同一寄存器不允许被两个 root parameter 占用，**除非
// ShaderVisibility 不重叠**"（之前静态采样器报的 E_INVALIDARG 就是这条规则，
// 报错文本里也带着 visibility）。所以正确解法是：**每阶段一个 CBV**，
// 各自沿用自己那份天然布局 —— 零合并、零改写、零猜测。
//
// ★ 为什么 SRV 范围不能直接用 sampler 个数
// ---------------------------------------------------------------------------
// vulkanize 给 sampler 分配 `layout(set=0, binding=1..N)`（0 留给 Globals），
// SPIRV-Cross **原样保留 binding 号**作为 HLSL 寄存器号，于是 N 个采样器占
// t1..tN（实测 dx12_pbr_frag.hlsl 里就是 t1..t25）。若把 NumDescriptors 设成
// N，覆盖的是 t0..t(N-1) → **最末一个 t_N 落在范围外**。
// 更别说 SSBO（readonly buffer → StructuredBuffer）也会占 t 寄存器
// （instanced.vert 的 dst_models）。所以这里直接扫生成的 HLSL 取 max+1。
//
// ---------------------------------------------------------------------------
// 三、一个 shader = 一个 PSO 集合，不是一个 PSO
// ---------------------------------------------------------------------------
// 引擎的渲染状态（深度测试/写入、混合、剔除）是**逐 draw 命令**下发的，而
// D3D12 的 PSO 把深度/混合/剔除全编译进去。Vulkan 侧同样如此，它的解法是
// `pipeline_cache_` / `pipeline_cache_opaque_`：按 (blend, depth_write, cull)
// 组合缓存 PSO，在 prepare_draw 时挑一个。
//
// 这里照搬同一思路：按 key 缓存 PSO。key 的组成见 PsoKey。
//
// ---------------------------------------------------------------------------
// 四、顶点输入布局从哪来
// ---------------------------------------------------------------------------
// D3D12 的 D3D12_INPUT_ELEMENT_DESC **按语义名匹配**（POSITION / NORMAL /
// TANGENT / TEXCOORD / COLOR），而不是 location。M4.1 已通过
// spvc_compiler_hlsl_add_vertex_attribute_remap 强制 HLSL 生成正确语义名
// （dxc -dumpbin 已验证得到 POSITION/NORMAL/TANGENT/TEXCOORD/COLOR），
// 所以这里只需按 mesh 的 VertexLayout（location + type + offset）走
// d3d12_mesh::location_to_semantic 生成 INPUT_LAYOUT，两边就能对上。
//
// 棘手点：**INPUT_LAYOUT 在 PSO 创建时就要确定**，而 mesh 是 draw 时才给的。
// 解法：load_program 时按"该着色器的标准顶点布局"预建 PSO；对引擎里那些
// 顶点格式不固定的着色器（line3d / particle / text3d 等），在第一次
// draw 时用 mesh 的实际布局**补建**一条 PSO（缓存 key 里带布局指纹）。
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "render/shader.h"
#include "render/mesh.h"
#include "d3d12_vulkanize.h"
#include "d3d12_reflect.h"

namespace gryce_engine::render::d3d12 {

class D3D12Backend;
class D3D12Mesh;
class D3D12Texture;

// ---------------------------------------------------------------------------
// 顶点输入布局指纹 —— 让 PSO 能按"实际顶点格式"缓存
// ---------------------------------------------------------------------------
// 两个 mesh 的 (location, type, offset, stride) 完全一致才算同一布局。
struct VertexLayoutKey {
    std::vector<uint32_t> packed;   // 逐属性 (location, type, offset) 三元组 + stride
    uint32_t stride = 0;

    bool operator==(const VertexLayoutKey& o) const {
        return stride == o.stride && packed == o.packed;
    }
};

struct VertexLayoutKeyHash {
    size_t operator()(const VertexLayoutKey& k) const noexcept {
        size_t h = std::hash<uint32_t>{}(k.stride);
        for (uint32_t v : k.packed) h = h * 1099511628211ull ^ std::hash<uint32_t>{}(v);
        return h;
    }
};

// ---------------------------------------------------------------------------
// PSO 缓存键 —— 深度/混合/剔除 + 顶点布局 + 渲染目标格式
// ---------------------------------------------------------------------------
struct PsoKey {
    VertexLayoutKey layout;
    uint32_t rt_format = 0;        // DXGI_FORMAT
    uint32_t ds_format = 0;        // DXGI_FORMAT（0 = 无深度缓冲）
    uint8_t  depth_test = 1;
    uint8_t  depth_write = 1;
    uint8_t  blend = 1;
    uint8_t  cull = 0;             // 0 = Back, 1 = None, 2 = Front
    uint8_t  primitive_topology = 0; // D3D12_PRIMITIVE_TOPOLOGY_TYPE 子集
    uint8_t  instance_stream = 0;  // 是否追加实例属性（location 5..8）

    bool operator==(const PsoKey& o) const {
        return layout == o.layout && rt_format == o.rt_format && ds_format == o.ds_format &&
               depth_test == o.depth_test && depth_write == o.depth_write && blend == o.blend &&
               cull == o.cull && primitive_topology == o.primitive_topology &&
               instance_stream == o.instance_stream;
    }
};

struct PsoKeyHash {
    size_t operator()(const PsoKey& k) const noexcept {
        size_t h = VertexLayoutKeyHash{}(k.layout);
        auto mix = [&h](size_t v) { h = h * 1099511628211ull ^ v; };
        mix(k.rt_format); mix(k.ds_format);
        mix((k.depth_test << 0) | (k.depth_write << 1) | (k.blend << 2) | (k.cull << 3) |
            (k.primitive_topology << 4) | (k.instance_stream << 6));
        return h;
    }
};

// 把 (顶点布局, 实例布局) 折成 PSO 缓存用的布局指纹。
// 供 D3D12Backend 在 draw 路径构造 PsoKey 使用。
VertexLayoutKey make_layout_key(const struct VertexLayout& layout,
                                const struct VertexLayout& instance_layout);

// ---------------------------------------------------------------------------
// D3D12Shader
// ---------------------------------------------------------------------------
class D3D12Shader final : public IShader {
public:
    D3D12Shader() = default;
    ~D3D12Shader() override;

    // 注入后端上下文。必须在 load_program 之前调用。
    void set_context(D3D12Backend* backend, ID3D12Device* device);

    // ---- IShader ----
    bool compile(const std::string& vertex_src, const std::string& fragment_src) override;
    bool compile(const std::vector<ShaderStageDesc>& stages) override;

    void bind() const override;
    void unbind() const override;

    void set_int(const std::string& name, int value) override;
    void set_int(const char* name, int value) override;
    void set_float(const std::string& name, float value) override;
    void set_float(const char* name, float value) override;
    void set_vec2(const std::string& name, const math::Vector2f& value) override;
    void set_vec2(const char* name, const math::Vector2f& value) override;
    void set_vec3(const std::string& name, const math::Vector3f& value) override;
    void set_vec3(const char* name, const math::Vector3f& value) override;
    void set_vec4(const std::string& name, const math::Vector4f& value) override;
    void set_vec4(const char* name, const math::Vector4f& value) override;
    void set_mat4(const std::string& name, const math::Matrix4f& value) override;
    void set_mat4(const char* name, const math::Matrix4f& value) override;

    void set_mat4_array(const char* name, const math::Matrix4f* data, uint32_t count) override;

    void set_texture(int slot, ITexture* texture) override;
    void set_pipeline_blending(bool enabled) override;

    // ★ 与 GL 版逐名对照（gl_shader.cpp:609）。D3D12 没有"延迟应用"的需要：
    //   直接按名字写进本帧 uniform 影子内存即可（write_uniform_raw 会按 std140
    //   偏移落在真正声明了该名字的阶段上）。
    //
    //   为什么这个方法必须存在（曾经的 DEVICE_HUNG 根因）：SSR 走的是 GLSL
    //   `ssr_trace.frag`（不像 Vulkan 有专门的 `vulkan_ssr_trace.frag` 用
    //   push constants 传参），它依赖 `uSSRNear/uSSRFar/uSSRMaxSteps` 等被显式
    //   赋值。缺了这一步，这些 uniform 恒为 0 → `linearize_depth` 用 uSSRNear*uSSRFar
    //   作分子、以 ~0 作分母 → NaN → DDA 循环里 px_total 变 NaN → 循环退化 →
    //   GPU 执行超时 → DXGI_ERROR_DEVICE_HUNG (TDR) 整设备丢失。
    void set_post_process_params(const PostProcessParams& params) override;

    bool load_program(const std::string& name, const std::string& shader_dir,
                      IFramebuffer* target = nullptr, bool color_output = true,
                      bool post_process = false, bool skybox = false,
                      bool skinned = false) override;

    bool is_valid() const override;

    // ---- D3D12 侧扩展 ----

    // 在给定 PSO 键下取（或建）一条管线。由 backend 的 draw 路径调用：
    //   * 逐个 draw 下发的 depth/blend/cull 会体现在 key 里；
    //   * 顶点布局指纹来自当前 mesh。
    // 返回 nullptr 表示 PSO 创建失败（已打日志）。
    ID3D12PipelineState* pipeline_for(const PsoKey& key);

    // 提交 draw 前的"把本帧 uniform 影子内存推到 GPU"。
    // 返回两个阶段各自的 CBV GPU 地址（无 uniform 块时该字段为 0）。
    struct UniformCbv {
        D3D12_GPU_VIRTUAL_ADDRESS vs = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ps = 0;
    };
    UniformCbv commit_uniforms();

    // 描述符表：SRV 段 / Sampler 段的 GPU 句柄。commit_textures() 之后有效。
    bool commit_textures();
    // 解析本帧按 slot 延迟记下的纹理绑定（见 .cpp 的 set_texture 说明）。
    void resolve_pending_textures();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table() const { return srv_table_gpu_; }
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table() const { return sampler_table_gpu_; }
    bool has_textures() const { return srv_register_count_ > 0 || sampler_register_count_ > 0; }
    bool has_srv_table() const { return srv_register_count_ > 0; }
    bool has_sampler_table() const { return sampler_register_count_ > 0; }

    // root parameter 索引：0 = CBV(VS) / 1 = CBV(PS)，
    // 之后按"是否存在"依次排 SRV 表 / Sampler 表（浮动的，不能硬编码）。
    uint32_t root_param_srv() const { return 2; }
    uint32_t root_param_sampler() const { return has_srv_table() ? 3u : 2u; }

    // 每帧开始：清空 uniform 影子内存与纹理绑定记录。
    // ★ 由**本帧第一次写 shader 状态时**惰性调用（见 .cpp 的 ensure_frame 说明）。
    void on_begin_frame(uint64_t frame_id);

    // ★★ "每帧重置"的惰性触发点 —— 整个 D3D12 后端最容易踩的时序坑 ★★
    // 本引擎的 RenderContext 是命令缓冲，一帧的调用顺序是
    //     begin_frame() → 逐条 dispatch(set_uniform_* / set_texture …) → 逐条
    //     dispatch(DrawMesh) → end_frame()
    // 即 **状态设置先于 draw**。所以"每帧重置"（清 uniform 影子内存、清纹理绑定、
    // 归还描述符区间）必须发生在"本帧第一条状态设置之前"，绝不能在 draw 里做
    // ——那样会把本帧刚设好的状态全部擦掉：所有 SRV 落到空描述符，采样恒返回 0，
    // 症状就是**整帧纯黑**（而深度/清屏仍然正常，因为不依赖采样）。
    // 这里用 backend 的帧号判断，故每个会写状态的入口都先调一次本函数（幂等）。
    void ensure_frame();

    // 该 shader 的顶点输入语义（location → 语义名 / 索引），供 backend 建
    // INPUT_LAYOUT 与"实例流追加属性"使用。
    const std::vector<uint32_t>& vertex_input_locations() const { return vertex_input_locations_; }
    const std::vector<std::string>& vertex_input_semantics() const { return vertex_input_semantics_; }

    // 某个 sampler 名对应的 SRV 表索引（t 寄存器号）。找不到返回 -1。
    int sampler_index_for_name(const std::string& name) const;

    // 供 backend 的 set_texture(unit, ITexture*) 直接按描述符索引绑定。
    void bind_texture_at_index(int index, D3D12Texture* tex);

    // ★ 把一个 SSBO 绑到本 shader 的 SRV 表里（按 **GLSL 块名**）。
    //   用途：GPU 剔除的实例流 —— `instanced.vert` 里
    //   `layout(std430, binding=2) readonly buffer DstModels { mat4 dst_models[]; };`
    //   在 HLSL 里是一个 StructuredBuffer，占一个 t 寄存器。它不在 sampler_names_
    //   里（那是纹理采样器），所以必须单独登记，否则该槽位是空描述符 → 顶点着色器
    //   读到全零矩阵 → 所有实例塌到原点（画面里只剩一团）。
    //   由 backend 在每次间接绘制前调用（每帧重新登记）。
    void set_external_buffer_srv(const char* block_name, ID3D12Resource* res, uint64_t size_bytes);
    // 每帧清空外部 SSBO 绑定（与 uniform/纹理绑定同生命周期）。
    void clear_external_buffer_srvs() { external_srv_.clear(); }
    // 本 shader 反射出的 SSBO 块（块名 → 寄存器 / UAV 标志 / 步长），诊断用。
    const std::vector<SsboBinding>& ssbo_bindings() const { return ssbo_bindings_; }

    // 当前是否为 skybox 管线（深度 LESS_EQUAL、不写深度、不剔除）。
    bool skybox() const { return skybox_; }
    const std::string& debug_name() const { return debug_name_; }

    // set_pipeline_blending 的当前值（draw 时进 PSO 缓存键）。
    bool blending() const { return blending_; }

    // root signature（draw 时 SetGraphicsRootSignature）。
    ID3D12RootSignature* root_signature() const { return root_signature_; }

private:
    // ★ 必须先于任何以它为参数的成员函数声明 —— 类内嵌套类型的名字查找
    //   不像函数体那样有"完整类上下文"的宽限。
    struct UniformEntry {
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t array_count = 0;   // 0 = 非数组或表达式维度
        uint32_t stride = 0;        // 数组元素步长（std140）
    };

    // ★ 每个着色阶段一份 Globals 块（见头文件顶部"为什么 CBV 要分两个"）。
    struct StageUniforms {
        std::unordered_map<std::string, UniformEntry> table;
        std::vector<uint8_t> shadow;      // 每帧写入的 CPU 影子内存
        std::vector<uint8_t> defaults;    // 复位快照（零 + 已求值的 GLSL 初始化式）
        uint32_t block_size = 0;          // std140 自然大小（未 256 对齐）
        bool dirty = false;
        bool present = false;             // 该阶段是否声明了 Globals 块
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;  // 本 draw 已申请的上传区间
    };

    // 把 GLSL 源码走完整条 D3D12 着色器链，产出 DXIL 字节码 + 布局元数据。
    bool build_stage(const std::string& glsl_source, GlslVariant variant,
                     std::vector<uint8_t>& out_dxil, std::string& error);

    // 建立**某个阶段**的 Globals 块状态：std140 查表 + 影子内存 + 默认值快照。
    void setup_stage_uniforms(StageUniforms& su, const VulkanizedShader& vz);

    // 按 std140 偏移写值；两个阶段各自尝试（同名可能只在一个阶段声明）。
    void write_uniform_raw(const char* name, const void* src, uint32_t size);

    // 解析 "uLightPos[3]" → ("uLightPos", 3)；非数组返回 false。
    bool split_array_index(const char* name, std::string& base, int& index) const;

    // 建 root signature（一次，与 PSO 数量无关）。
    bool create_root_signature(uint32_t srv_count, uint32_t sampler_count);

    // 建一条 PSO。key 决定状态，layout 决定顶点输入。
    ID3D12PipelineState* create_pso(const PsoKey& key);

    // ---- 上下文 ----
    D3D12Backend* backend_ = nullptr;
    ID3D12Device* device_ = nullptr;

    // ---- 着色器链产物 ----
    std::vector<uint8_t> vs_dxil_;
    std::vector<uint8_t> fs_dxil_;
    // 最近一次 build_stage 的 vulkanize 结果（含该阶段的 std140 字段表），
    // 由 compile() 用来建设对应阶段的 uniform 状态。
    VulkanizedShader last_vz_;
    std::vector<std::string> sampler_names_;
    // ★ SRV 根参数要覆盖的寄存器范围上界（t0..tN-1）。
    //   见头文件顶部说明：N 个采样器占 t1..tN，SSBO 还会额外占 t，
    //   所以这个值由"扫生成的 HLSL 取 max(register(tX)) + 1"得出。
    uint32_t srv_register_count_ = 0;
    // Sampler 表范围上界（同上，扫 register(sX)）。
    uint32_t sampler_register_count_ = 0;
    bool has_vs_uniforms_ = false;
    bool has_ps_uniforms_ = false;
    std::vector<std::string> vertex_input_semantics_;
    std::vector<uint32_t> vertex_input_locations_;

    StageUniforms vs_u_;
    StageUniforms ps_u_;

    // ---- 纹理绑定 ----
    struct BoundTexture {
        D3D12Texture* tex = nullptr;
        uint32_t srv_slot = 0;   // 在 SRV 堆里占的槽位
        uint32_t smp_slot = 0;   // 在 Sampler 堆里占的槽位
        bool allocated = false;
    };
    std::vector<BoundTexture> bound_textures_;   // 与 sampler_names_ 同长
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table_gpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table_gpu_{};
    // 本帧已申请的两段连续描述符区间（每帧末统一归还）。描述符表必须连续，
    // 且表首对应寄存器 t0 / s0 —— 采样器在第 (1+i) 个槽位（见 .cpp 说明）。
    // ★ 用 vector 而不是单个 base/count：同一 shader 一帧内可能因材质切换
    //   多次重建表（D3D12 的描述符表被已录制的命令引用，不能原地覆盖），
    //   只记最后一次会漏掉前面那些段的归还 → 堆被逐步耗尽。
    struct DescriptorRun {
        uint32_t base = 0;
        uint32_t count = 0;
        // 采样器区间（与 srv 区间成对分配；指纹复用时两者一起取回）。
        uint32_t smp_base = 0;
        uint32_t smp_count = 0;
    };
    std::vector<DescriptorRun> srv_runs_;
    std::vector<DescriptorRun> sampler_runs_;
    // ★ 本帧"纹理集合指纹 → 已建好的描述符区间"缓存。
    // 只按"连续两次绑定相同"复用是不够的（A B A C A 会为 A 建 3 次表），
    // 而 pbr 一次要吃 26 个 sampler 槽位 —— sampler 堆硬上限只有 2048，
    // 重复建表会把它撑爆 → 运行期扩容 → 已录制的表句柄全悬空。
    std::unordered_map<uint64_t, DescriptorRun> run_cache_;
    uint64_t texture_set_fingerprint() const;
    // 当前这张表绑定的纹理集合（用于"绑定没变就复用表"的判断）。
    std::vector<D3D12Texture*> bound_signature_;
    bool textures_dirty_ = false;

    // ---- 外部 SSBO（GPU 剔除的实例流）----
    // 本 shader 各阶段反射出的 SSBO 块（build_stage 时合并）。
    std::vector<SsboBinding> ssbo_bindings_;
    struct ExternalSrv {
        ID3D12Resource* res = nullptr;
        uint64_t size = 0;
    };
    std::unordered_map<std::string, ExternalSrv> external_srv_;
    // 当前 SRV 表的起始槽位（表内第 k 槽 ↔ 寄存器 t{k}），写外部 SRV 时要用。
    uint32_t current_srv_base_ = 0;
    // 把 external_srv_ 写进表里对应的 t 寄存器槽位。
    void apply_external_buffer_srvs(uint32_t srv_base);

    // ---- 引擎槽位（TextureSlots::kXxx）→ GLSL 采样器名 ----
    // ★ 为什么需要这张表（这是"画面全黑"的根因，见 .cpp 的详解）：
    //   引擎的 set_texture 绝大多数调用把 uniform 名传成**空串**
    //   （material.cpp:307、render_pipeline.cpp 里几十处 "..."），
    //   名字是在**紧随其后**的 set_uniform_int(<采样器名>, <同一个 slot>) 里才出现的。
    //   所以"靠 set_texture 紧邻的前一个 set_int 认名字"（pending_sampler_name_）
    //   对绝大多数绑定根本不成立，纹理被整个丢掉 → 所有 SRV 都是空描述符 → 全黑。
    //   正确做法：set_texture 先按 **slot** 记下待绑纹理，等名字从任一渠道
    //   （本帧的 set_int / 上次学到的）补齐后，在 commit_textures() 时统一落地。
    std::unordered_map<int, std::string> slot_sampler_name_;
    // 本帧按 slot 延迟待绑定的纹理（空名绑定的落点，commit 时解析）。
    std::unordered_map<int, D3D12Texture*> pending_by_slot_;
    // set_int(<采样器名>, unit) 与**紧接**的 set_texture(unit, tex) 构成精确配对时，
    // 用它当场落位（不必等到 commit）。这是"名字先行"的命名绑定路径。
    std::optional<std::string> pending_sampler_name_;

    // ---- 管线 ----
    ID3D12RootSignature* root_signature_ = nullptr;
    std::unordered_map<PsoKey, ID3D12PipelineState*, PsoKeyHash> pso_cache_;
    ID3D12PipelineState* pso_ = nullptr;   // 最近一次用过的 PSO

    // ---- 状态 / 元数据 ----
    std::string name_;
    std::string shader_dir_;
    bool color_output_ = true;
    bool post_process_ = false;
    bool skybox_ = false;
    bool skinned_ = false;
    bool blending_ = true;      // set_pipeline_blending
    bool valid_ = false;
    std::string debug_name_;
    // 惰性每帧重置用的帧号（0 表示"从未重置过"）。
    uint64_t last_frame_id_ = 0;
};

} // namespace gryce_engine::render::d3d12
