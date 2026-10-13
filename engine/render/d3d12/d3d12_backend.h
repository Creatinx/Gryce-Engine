// =============================================================================
// d3d12_backend — D3D12 后端
//
// 已实现范围（M0–M6，见 docs/d3d12-backend-plan.md 的「实施现状」）：
//   * 设备/交换链/帧循环：loader（运行时加载 d3d12.dll/dxgi.dll）、适配器与设备、
//     命令队列/分配器/列表、帧栅栏、RTV/DSV 堆、FLIP_DISCARD 交换链、
//     录制-提交-Present、截图回读。
//   * 资源层：描述符堆管理器（CBV_SRV_UAV + Sampler）、上传路径（staging /
//     UploadRing）、Buffer、Texture（mip / cubemap / 压缩 / HDR / 深度）、Mesh、
//     Framebuffer，以及五个 RHIResourcePool 的接线。
//   * 着色器链：GLSL →（清 vulkan 方言 → SPIR-V）→ HLSL（SPIRV-Cross）→ DXIL（DXC），
//     root signature + PSO 缓存、按名字的 set_* uniform（root constants/UBO）、
//     按 slot/名字的纹理绑定、compute 管线与间接绘制。
//   * 2D 渲染器（复用 renderer2d_impl）、ImGui 后端、与 RenderPipeline 的端到端联调。
//
// ★ Y 方向（最容易踩的一条，见 .cpp 里 ensure_present_target 的详解）：
//   翻译层用 FLIP_VERTEX_Y 把整条管线等效成 OpenGL 约定，因此"画到屏幕"的 pass
//   全部落在内部 present 目标上，帧末由一条自带 blit 通道翻一次写进真实 back buffer。
//   不要再尝试"关掉 FLIP_VERTEX_Y 让几何方向天然正确"—— 那会让每个全屏 pass 都多翻
//   一次，画面方向随启用的 pass 变化，且着色器里 GL 约定的 V 翻转全部失效。
// =============================================================================
#pragma once

// D3D12 与 DXGI 头。IDXGIFactory4 在 dxgi1_4.h（不是 d3d12.h 或 dxgi.h），
// 缺了它会在 GCC 下报 "'IDXGIFactory4' does not name a type"。
// mingw-w64 vendor 头与 Windows SDK 头都提供这两个文件。
#include <d3d12.h>
#include <dxgi1_4.h>
#include <array>
#include <string>

#include "render/render.h"
#include "render/rhi_resource_pool.h"
#include "render/gpu_profiler.h"
#include "d3d12_loader.h"
#include "d3d12_swapchain.h"
#include "d3d12_descriptors.h"
#include "d3d12_upload.h"
#include "d3d12_buffer.h"
#include "d3d12_texture.h"
#include "d3d12_mesh.h"
#include "d3d12_framebuffer.h"
#include "d3d12_shader.h"
#include "d3d12_compute.h"

namespace gryce_engine::render::d3d12 {

class D3D12Backend final : public IRenderBackend, public D3D12Buffer::UploadSink,
                           public ResourceRetireSink {
public:
    D3D12Backend() = default;
    ~D3D12Backend() override;

    // ---- 生命周期 ----
    bool init(void* native_window) override;
    void shutdown() override;

    // ---- 上下文管理（D3D12 无"当前上下文"：交换链按窗口创建）----
    void make_current(void* native_window) override;
    void release_context() override;

    // ---- 帧循环 ----
    void begin_frame() override;
    void end_frame() override;
    void wait_gpu_idle() override;

    // ---- 状态 / 绘制 ----
    void clear(float r, float g, float b, float a) override;
    void clear_depth() override;
    void set_viewport(int x, int y, int w, int h) override;
    void set_scissor(int x, int y, int w, int h) override;
    void set_depth_test(bool enabled) override;
    void set_depth_write(bool enabled) override;
    void set_blend(bool enabled) override;
    void set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) override;
    void set_blend_equation(BlendEquation mode) override;
    void set_cull_face(CullMode mode) override;
    void     draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) override;
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) override;

    // ---- 计算着色器 / 间接绘制（M6：GPU 剔除）----
    // GPU-driven 渲染：compute 视锥/HiZ 剔除 → 紧凑写入实例 SSBO + 间接参数，
    // 绘制端 ExecuteIndirect 提交（CPU 不再逐物体参与）。语义与 GL/Vulkan 一致。
    bool supports_compute_shaders() const override;
    RHIShaderHandle create_compute_shader(const char* name, const char* shader_dir) override;
    void dispatch_compute(RHIShaderHandle shader, uint32_t gx, uint32_t gy, uint32_t gz) override;
    void bind_storage_buffer(RHIShaderHandle shader, const char* name,
                             RHIBufferHandle buffer) override;
    void bind_instance_stream(RHIBufferHandle buffer) override;
    void draw_indexed_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                               RHIBufferHandle args, uint32_t offset) override;
    void draw_arrays_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                              RHIBufferHandle args, uint32_t offset) override;
    void copy_texture_to_buffer(RHITextureHandle texture, RHIBufferHandle buffer,
                                uint32_t offset) override;

    // ---- 帧率控制 ----
    void set_swap_interval(int interval) override;

    // ---- 资源工厂 / 句柄解析（M3 已接线）----
    RHIMeshHandle create_mesh() override;
    RHIShaderHandle create_shader() override;
    RHITextureHandle create_texture() override;
    RHIFramebufferHandle create_framebuffer() override;
    RHIBufferHandle create_buffer() override;

    void destroy_mesh(RHIMeshHandle handle) override;
    void destroy_shader(RHIShaderHandle handle) override;
    void destroy_texture(RHITextureHandle handle) override;
    void destroy_framebuffer(RHIFramebufferHandle handle) override;
    void destroy_buffer(RHIBufferHandle handle) override;

    IMesh* mesh(RHIMeshHandle handle) override;
    IShader* shader(RHIShaderHandle handle) override;
    ITexture* texture(RHITextureHandle handle) override;
    IFramebuffer* framebuffer(RHIFramebufferHandle handle) override;
    IBuffer* buffer(RHIBufferHandle handle) override;

    // ---- 帧缓冲绑定（M3）----
    void bind_framebuffer(RHIFramebufferHandle fb) override;
    void unbind_framebuffer() override;

    // ---- 元信息 ----
    const char* api_name() const override;
    const char* api_version() const override;
    RenderBackendCapabilities get_capabilities() const override;
    // MSAA resolve：把 src 帧缓冲的多样本颜色 ResolveSubresource 到 dst 的
    // 单样本颜色附件。两端均非多样本时直接返回（无意义）。
    void resolve_framebuffer(RHIFramebufferHandle src, RHIFramebufferHandle dst) override;

    // ---- GPU 分段计时（M6 调试用；D3D12 时间戳查询）----
    //   D3D12 用 ID3D12QueryHeap(TIMESTAMP) + ResolveQueryData 写进一段
    //   READBACK 缓冲。与 GL/Vulkan 后端一致：区间平铺、不阻塞、按帧槽轮转，
    //   在"槽被复用"时才回读结算。
    //   ★ 这套计时是为了定位 DEVICE_HUNG 卡在哪一段 —— 设备挂起后 GPU 时间戳
    //     不再推进，读回可看到"最后一个完成的 pass"，从而把 TDR 钉到具体段。
    bool gpu_profile_supported() const override { return true; }
    void set_gpu_profiling(bool enabled) override;
    void gpu_profile_begin(const char* name) override;
    void gpu_profile_end() override;
    void gpu_profile_frame_boundary() override;
    void gpu_profile_dump() override;
    void gpu_profile_reset() override;
    double gpu_frame_time_ms() const override;

    // ---- 2D / UI 工厂（M5）----
    std::unique_ptr<IRenderer2D> create_renderer2d() override;
    std::unique_ptr<IImGuiBackend> create_imgui_backend() override;
    // ImGui 绘制后要把引擎自己的描述符堆重新绑回去（D3D12 同一时刻只允许一个
    // shader-visible SRV 堆被绑定，ImGui 会替换掉它）。设置 nullptr 表示回到引擎主堆。
    void rebind_descriptor_heaps();

    // ★ 把某张纹理在当前帧命令列表上转到 "shader 可读"（PS_SRV | NON_PS_SRV）。
    //   D3D12 采样一张仍处于 RENDER_TARGET / DEPTH_WRITE 态的资源是**非法**的：
    //   调试层会静默容忍，但真机会读到未定义内容，Intel 上甚至直接挂起（TDR）。
    //   Vulkan 靠 render pass 的 finalLayout 自动完成这一步；D3D12 没有等价机制，
    //   必须在"把它当 SRV 绑定"时显式补屏障。
    //   幂等：已是目标态则什么都不做。
    void ensure_texture_shader_readable(class D3D12Texture* tex);

    // ---- 截图（M5.3）----
    void request_screenshot(const std::string& path) override;
    bool capture_frame_rgba(std::vector<uint8_t>& out, int& width, int& height) override;

    // ---- 调试探针（SSR 竞态排查用，env GRYCE_DX12_PROBE_SSR=1 门控）----
    // 在指定帧把纹理拷回 CPU，输出均值 / 非零占比 / 内容哈希。
    void probe_texture(RHITextureHandle tex, const char* tag) override;

    // ★ ImGui（或其它自带 D3D12 原生约定的覆盖层）绘制前调用：
    //   把本帧的 present 目标内容先翻转 blit 进真实 back buffer，并把 back buffer
    //   设为当前 RT —— 覆盖层随后直接画在"已纠正方向"的成品之上。
    //   （引擎自身的 2D/3D pass 走 GL 约定，必须落在 present 目标里一起被翻转；
    //     ImGui 的内置着色器是 D3D 原生约定，翻转后反而会上下颠倒。）
    void flush_present_blit_for_overlay();

    // ---- D3D12 侧扩展（供 shader / pipeline 层使用）----
    ID3D12Device* device() const { return device_; }
    ID3D12CommandQueue* command_queue() const { return queue_; }
    D3D12Swapchain& swapchain() { return swapchain_; }
    DescriptorHeapSet& descriptor_heaps() { return descriptor_heaps_; }
    UploadRing& upload_ring() { return upload_ring_; }
    // 动态加载的函数指针（如 D3D12SerializeRootSignature），供 shader 层使用。
    const Loader& loader() const { return loader_; }
    // ★ 当前帧号。shader 用它做"每帧重置"的惰性触发（见 D3D12Shader::ensure_frame）。
    //   ★★ 时序是这里的全部要点 ★★
    //   RenderContext 是**命令缓冲**：一帧内的调用顺序是
    //       begin_frame() → [逐个 dispatch 命令: set_uniform_* / set_texture …]
    //                        → [dispatch 到 DrawMesh → draw_mesh()]
    //                        → end_frame()
    //   也就是说 **shader 的 uniform/纹理设置发生在 draw 之前**。
    //   所以"每帧重置"绝不能在 draw_mesh 里做（那会把本帧刚设好的状态整块擦掉，
    //   症状就是"所有 SRV 都是空描述符 → 采样恒 0 → 画面全黑"）。
    //   正确触发点是**本帧第一次写 shader 状态时**（或 bind() 时），见 ensure_frame()。
    uint64_t current_frame_id() const { return frame_counter_; }

private:
    void release_device_objects();
    // ★ 延迟释放：把池槽位记进 retire 队列，等 GPU 跑过记录时的围栏值再真正
    //   deallocate。D3D12 严禁销毁仍被"已提交但未完成"的命令列表引用的资源
    //   （调试层报 "resource ... deleted prior to executing the command list"，
    //   并且实测会连锁触发 DXGI_ERROR_DEVICE_HUNG）。
    //   典型受害者：effect 的离屏目标（SSR blur/composite、bloom 各级）在
    //   resize / scale 变化时被 create_targets 重建，旧纹理当帧就被销毁。
    void retire_texture(uint32_t index, uint32_t generation);
    void retire_framebuffer(uint32_t index, uint32_t generation);
    void retire_mesh(uint32_t index, uint32_t generation);
    void retire_buffer(uint32_t index, uint32_t generation);
    // 每次 begin_frame 调一次：把已完成围栏值之前的 retire 项真正释放。
    void drain_retired();
    // ---- present 目标 + 帧末翻转 blit（见 d3d12_backend.cpp 的详解）----
    // 为什么需要：本后端用 SPIRV-Cross 的 FLIP_VERTEX_Y 把整条管线等效成 OpenGL
    // 的坐标/纹理约定（这样引擎所有 GL 版着色器里的 V 翻转、屏幕空间 UV 计算
    // 都原样成立，实测画面与 GL 逐像素一致）。代价是**最终成品在 D3D 的呈现
    // 方向上是上下镜像的**：D3D 的 back buffer 第 0 行在屏幕顶部，而 GL 约定的
    // 第 0 行在屏幕底部。所以引擎所有"画到屏幕"的 pass 先落在后端内部的
    // present 目标上，帧末再用一条自带的 blit 通道翻一次写进真实 back buffer。
    //   * 这样翻转只发生一次，且与"这一帧开了哪些 pass"无关（早先把 FLIP_VERTEX_Y
    //     关掉时，镜像次数会随 pass 数量变化 → 开/关 SSR 画面方向都不一样）。
    bool ensure_present_target();
    void release_present_target();
    bool ensure_blit_pipeline();
    void record_present_blit(ID3D12GraphicsCommandList* cmd);
    // 当前"屏幕 RT"的 RTV：present 目标（未 blit）或真实 back buffer（已 blit）。
    D3D12_CPU_DESCRIPTOR_HANDLE screen_rtv() const;
    // 把当前帧的 clear 录进命令列表（含 back buffer 状态转换）。
    void record_clear_if_dirty();
    // 把 RT 切回交换链 back buffer（idempotent：已是当前 RT 时不做重复屏障）。
    // bind_framebuffer 解析失败时也走这里 —— 见 GL 的同名语义。
    void bind_swapchain_rt();
    // 输出 D3D12 调试层的校验消息（仅 GRYCE_D3D12_DEBUG=1 时有内容）。
    void dump_info_queue();
    // ---- 截图（M5.3）两阶段 ----
    //   begin_screenshot_copy 在 end_frame 里被调用，把纹理→回读缓冲的拷贝
    //   **录制进本帧已有的命令列表**（必须和本帧绘制同一条列表，否则拷到空
    //   buffer → 确定性黑帧）。finish_screenshot 在本帧提交并等 GPU 后调用，
    //   从回读缓冲取像素并落盘。
    bool begin_screenshot_copy(ID3D12GraphicsCommandList* cmd);
    void finish_screenshot();
    void release_screenshot_staging();
    // 调试探针：帧提交后等 GPU、回读统计并打日志（见 probe_texture）。
    void probe_drain();
    struct PendingProbe {
        ID3D12Resource* readback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t total = 0;
        uint32_t w = 0, h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        const char* tag = nullptr;
        uint64_t frame = 0;
    };
    std::vector<PendingProbe> pending_probes_;
    // native_window 是 GLFWwindow*（见 render_context.cpp:157），D3D12 需要真实
    // HWND —— 走 glfwGetWin32Window 转换。非 GLFW 句柄时返回 nullptr。
    static HWND hwnd_from_native(void* native_window);

    // ---- 计算 / 间接绘制的私有支撑 ----
    // D3D12Buffer::UploadSink：把 CPU 数据拷进 DEFAULT 堆 SSBO（录制进当前帧命令
    // 列表，不走"提交+等待"，避免每帧 N 次 GPU 停顿）。
    bool record_copy(ID3D12Resource* dst, uint64_t dst_offset, const void* data, size_t size,
                     D3D12_RESOURCE_STATES& inout_state) override;
    // 资源状态翻转（幂等）。UPLOAD 堆资源（GENERIC_READ）不可翻转，直接跳过。
    void transition_buffer(D3D12Buffer* b, D3D12_RESOURCE_STATES after);
    // 惰性创建间接绘制的命令签名（DrawInstanced / DrawIndexedInstanced 两种）。
    bool ensure_command_signatures();
    // draw_mesh / draw_indexed 与间接版的公共实现（间接版 args 有效）。
    void draw_indirect_impl(RHIMeshHandle mesh, RHIShaderHandle shader, RHIBufferHandle args,
                            uint32_t offset, bool indexed);
    // 绑定顶点输入槽：slot 0 = 顶点流，slot 1 = 实例流（如果该网格有）。
    // ★ D3D12 要求"同一 InputSlot 内分类一致"，所以实例元素必须落 slot 1
    //   （见 d3d12_shader.cpp create_pso 的说明）；没有实例流时显式写入空 view
    //   清掉上一帧的绑定，避免泄漏给下一个 PSO。
    void bind_vertex_slots(ID3D12GraphicsCommandList* cmd, D3D12Mesh* m);
    D3D12ComputeShader* compute_shader(RHIShaderHandle h);
    // ★ 计算句柄用 generation 最高位做类型标记（与 Vulkan 后端同一约定）：
    //   图形与计算是两个池，索引会撞车（都从 0 开始），必须靠标志位分辨，
    //   绝不能"两个池都查一遍看哪个活着"。
    static constexpr uint32_t kComputeShaderFlag = 0x80000000u;
    static bool is_compute_shader(RHIShaderHandle h) {
        return (h.generation & kComputeShaderFlag) != 0;
    }
    static uint32_t compute_pool_generation(RHIShaderHandle h) {
        return h.generation & ~kComputeShaderFlag;
    }

    Loader loader_{};
    IDXGIAdapter1*  adapter_ = nullptr;
    ID3D12Device*   device_  = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;

    D3D12Swapchain swapchain_{};

    // ---- M3 资源层 ----
    DescriptorHeapSet descriptor_heaps_{};
    UploadRing upload_ring_{};

    RHIResourcePool<D3D12Mesh> mesh_pool_;
    RHIResourcePool<D3D12Texture> texture_pool_;
    RHIResourcePool<D3D12Framebuffer> framebuffer_pool_;
    RHIResourcePool<D3D12Buffer> buffer_pool_;
    RHIResourcePool<D3D12Shader> shader_pool_;
    // 计算管线单独一个池（句柄靠 generation 最高位区分，见 kComputeShaderFlag）。
    RHIResourcePool<D3D12ComputeShader> compute_pool_;

    // ---- M6：GPU 剔除的绑定状态 ----
    // 本帧按 **GLSL 块名** 记下的 SSBO 绑定；dispatch_compute 时解析成
    // D3D12Buffer*、翻状态、写描述符。
    std::unordered_map<std::string, RHIBufferHandle> storage_bindings_;
    // bind_instance_stream 记下的"可见实例矩阵 SSBO"（供 instanced.vert 读）。
    RHIBufferHandle instance_stream_{};
    // 间接绘制命令签名（惰性创建并缓存）。
    ID3D12CommandSignature* sig_draw_ = nullptr;
    ID3D12CommandSignature* sig_draw_indexed_ = nullptr;

    // ---- HiZ 纹理→SSBO 的延迟重排（见 d3d12_backend.cpp copy_texture_to_buffer）----
    // D3D12 的 CopyTextureRegion 要求 256 对齐行距，而引擎的 HiZ SSBO 是紧凑行，
    // 所以先按对齐行距拷进 READBACK 暂存，等 GPU 跑完（围栏）后下一帧在 CPU 上
    // 重排成紧凑行再写回 SSBO。
    struct PendingHizCopy {
        ID3D12Resource* staging = nullptr;   // READBACK，行距 pitch
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t pitch = 0;
        RHIBufferHandle dst{};
        uint64_t dst_offset = 0;
        uint64_t fence = 0;                  // 该拷贝提交时的围栏值
    };
    std::vector<PendingHizCopy> pending_hiz_;
    void drain_hiz_repacks();

    // ---- 延迟释放队列（见 retire_* 声明处的说明）----
    struct RetiredResource {
        uint64_t fence_value = 0;   // 记录时"已交给 GPU"的围栏值
        uint32_t index = 0;
        uint32_t generation = 0;
    };
    std::vector<RetiredResource> retired_textures_;
    std::vector<RetiredResource> retired_framebuffers_;
    std::vector<RetiredResource> retired_meshes_;
    std::vector<RetiredResource> retired_buffers_;

public:
    // ★ 描述符区间的延迟归还（这是 D3D12 上"开 SSR 就 DEVICE_HUNG"的根因）。
    //
    //   症状：引擎有 frames_in_flight(=3) 个帧槽，渲染线程投递后 GPU 会落后
    //   2~3 帧。而 D3D12Shader::ensure_frame() 在**本帧首次使用该 shader 时**
    //   就把上帧申请的 SRV/Sampler 描述符区间 free_range 掉，紧接着把它重新
    //   分配给别的纹理 —— 但"上帧的命令列表"此刻 GPU 可能**还在执行**。
    //   于是 GPU 去读那段描述符时，内容已经被改写成了别的纹理（或空描述符）
    //   → Intel Arc 上直接 GPU 挂起 → DXGI_ERROR_DEVICE_HUNG。
    //
    //   为什么只有"SSR + shadow 全开"才复现：描述符是零和资源，只有每帧用量
    //   足够大（SSR 三个 pass 各要一段表 + pbr 要 26 个 sampler 槽）时才必然
    //   发生区间复用；而挂起迟早取决于 GPU 落后帧数，所以"shadow 越大挂得越早"
    //   （3072→frame12，4096→frame5）。--no-ssr / --no-shadow 少了那段用量，
    //   碰巧不触发。
    //
    //   修法：归还也按围栏值排队，GPU 真正跑过那一帧之后才 free_range。
    void retire_descriptor_range(uint32_t srv_base, uint32_t srv_count,
                                 uint32_t smp_base, uint32_t smp_count);
    // D3D12Mesh::ResourceRetireSink：网格缓冲被替换（容量不足而重建）时，把旧资源
    // 挂到"最近一次已提交"的围栏值上，等 GPU 跑完那几帧再真正 Release。
    // 直接 Release 会让仍被在飞命令列表引用的资源消失 → RemoveDevice/DEVICE_HUNG。
    void retire_gpu_resource(ID3D12Resource* res) override;
    // 当帧"刚提交、尚未完成"的围栏值（归还请求要挂在这个值上）。
    uint64_t pending_fence_value() const { return swapchain_.last_submitted_value(); }

private:
    struct RetiredDescriptors {
        uint64_t fence_value = 0;
        uint32_t srv_base = 0;
        uint32_t srv_count = 0;
        uint32_t smp_base = 0;
        uint32_t smp_count = 0;
    };
    std::vector<RetiredDescriptors> retired_descriptors_;
    // 被替换掉的网格缓冲（同上：按围栏值延迟释放）。
    struct RetiredRawResource {
        uint64_t fence_value = 0;
        ID3D12Resource* res = nullptr;
    };
    std::vector<RetiredRawResource> retired_raw_resources_;

    // ---- GPU 分段计时（时间戳查询）----
    // 每帧槽一套 query heap 区间；k_gpu_slots = frames_in_flight(3) + 1 余量，
    // 结算"两帧前的槽"时 GPU 必然已跑完。
    static constexpr int k_gpu_slots = 4;
    static constexpr int k_gpu_max_scopes = 32;
    static constexpr int k_gpu_ts_per_slot = k_gpu_max_scopes * 2;   // 每段 begin+end

    bool ensure_gpu_profiling_objects();
    void release_gpu_profiling_objects();
    // 设备挂起时把还挂在飞的槽立刻 Resolve+读回（TDR 定位用）。
    void gpu_profile_flush_pending();

    bool gpu_profiling_enabled_ = false;
    bool gpu_profiling_ready_ = false;
    int  gpu_slot_ = 0;
    int  gpu_pending_scope_ = -1;
    bool gpu_nested_warned_ = false;
    // 每槽：begin/end 时间戳数（2×段数）与段名。
    std::array<int, k_gpu_slots> gpu_scope_count_{};
    std::array<std::array<std::string, k_gpu_max_scopes>, k_gpu_slots> gpu_scope_names_{};
    // 一个 QueryHeap 覆盖全部槽（每槽一段连续区间），配一段 READBACK 缓冲。
    ID3D12QueryHeap* gpu_query_heap_ = nullptr;
    ID3D12Resource*  gpu_readback_ = nullptr;
    uint64_t         gpu_timestamp_freq_ = 0;
    // 每槽已为哪个"提交围栏值"发过 ResolveQueryData（0 = 本槽尚无待结算数据）。
    std::array<uint64_t, k_gpu_slots> gpu_slot_fence_{};
    uint64_t         gpu_last_drained_fence_ = 0;
    // 设备挂起路径专用的 flush 围栏（不复用 swapchain 的 fence，避免干扰帧槽）。
    ID3D12Fence*     gpu_flush_fence_ = nullptr;
    uint64_t         gpu_flush_fence_value_ = 0;
    GpuProfilerStats gpu_stats_;

    // 当前绑定的帧缓冲（nullptr 表示渲染到交换链 back buffer）。
    RHIFramebufferHandle bound_framebuffer_{};

    HWND hwnd_ = nullptr;
    bool initialized_ = false;
    bool frame_aborted_ = false;
    bool device_lost_ = false;
    // 调试开关 GRYCE_D3D12_SERIALIZE=1：每帧末等 GPU 跑完（TDR 定位用）。
    bool gpu_serialize_ = false;
    bool debug_layer_enabled_ = false;
    // 单调递增的帧号：shader 用它做惰性"每帧重置"，触发点在**本帧第一次写
    // shader 状态时**（不是 draw 时 —— 见 current_frame_id() 的时序说明）。
    uint64_t frame_counter_ = 0;
    // 本帧 back buffer 是否已被切到 RENDER_TARGET（决定 end_frame 是否要切回 PRESENT）
    bool back_buffer_as_rt_ = false;

    // ---- present 目标（画到屏幕的 pass 的真正落点）+ 帧末翻转 blit ----
    ID3D12Resource* present_tex_ = nullptr;              // 与交换链同尺寸/同格式
    uint32_t present_w_ = 0, present_h_ = 0;
    ID3D12DescriptorHeap* present_rtv_heap_ = nullptr;   // NON_SHADER_VISIBLE，1 槽
    D3D12_CPU_DESCRIPTOR_HANDLE present_rtv_{};
    D3D12_RESOURCE_STATES present_state_ = D3D12_RESOURCE_STATE_COMMON;
    bool present_dirty_ = false;      // 本帧有 pass 落在 present 目标上（需要 blit）
    bool present_flushed_ = false;    // 本帧已 blit 进真实 back buffer（UI 覆盖层之后）
    bool backbuffer_as_rt_ = false;   // 真实 back buffer 当前是否处于 RENDER_TARGET
    bool using_present_target_ = false;   // 当前绑定的"屏幕 RT"是否是 present 目标
    // 翻转 blit 的自带管线（VS 用 SV_VertexID 生成全屏三角形，PS 采样 present 目标）。
    // 顶点着色器**不做** Y 翻转 —— 恰是这一条让最终图像从 GL 约定转回 D3D 呈现方向。
    ID3D12RootSignature* blit_root_ = nullptr;
    ID3D12PipelineState* blit_pso_ = nullptr;
    bool blit_pipeline_failed_ = false;   // 创建失败后不再重试（每帧只警告一次）
    // blit 用的 SRV 描述符槽位：**按帧槽固定复用**（每槽一个），不是每帧新申请。
    // ★★ 绝不能每帧从共享堆里 allocate 一个不归还：D3D12 的 SHADER_VISIBLE 堆一旦
    //    被撑满，DescriptorAllocator 会**重建更大的堆并拷贝描述符** —— 已录进在飞
    //    命令列表的 GPU 句柄（绝对地址）立刻悬空，实测就是
    //      "CreateGraphicsPipelineState failed ... removed_reason=0x887A0006"
    //      （DEVICE_HUNG）然后整条链雪崩。短跑看不出，跑够帧数（8192/每帧 1 个）
    //    才爆 —— 这正是"只渲染了一帧画面是对的、后面就挂了"的成因。
    //    按帧槽复用是安全的：begin_frame 已经等过该槽的围栏值，用同一槽的帧
    //    必然已经跑完。
    std::vector<uint32_t> present_srv_slots_;   // 下标 = 帧槽；UINT32_MAX = 未分配

    // ---- 截图（M5.3）----
    // 与 GL/Vulkan 同构：request_screenshot 记下路径与目标帧号，end_frame 里
    // 帧号一到就把 back buffer 拷进回读缓冲；本帧提交并等 GPU 后取回像素落盘。
    std::string screenshot_path_;
    uint64_t screenshot_frame_ = 0;
    // 本帧实际渲染进的 back buffer 索引：在 bind_swapchain_rt 记下 —— 交换链有 3 个
    // back buffer 轮转，只有本帧绑定的那个持有本帧画面。
    uint32_t rendered_buffer_index_ = 0;
    // 截图待回读的中间对象（begin_screenshot_copy 填，finish_screenshot 消费）。
    ID3D12Resource* shot_readback_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shot_footprint_{};
    uint64_t shot_total_ = 0;
    uint32_t shot_w_ = 0;
    uint32_t shot_h_ = 0;

    // 当前帧待应用状态（D3D12 无"立即状态"，一切在录制时下发）
    float clear_r_ = 0.0f, clear_g_ = 0.0f, clear_b_ = 0.0f, clear_a_ = 1.0f;
    bool  clear_dirty_ = true;
    int   viewport_x_ = 0, viewport_y_ = 0, viewport_w_ = 0, viewport_h_ = 0;

    // ★ 当前绑定的渲染目标尺寸。D3D12 要求 **scissor 必须完全落在 RT 内**，
    // 越界会让驱动直接把设备移除（GetDeviceRemovedReason = DXGI_ERROR_INVALID_CALL，
    // 之后所有调用固定返回 0x887A0005，不可恢复）。所以 set_viewport/set_scissor
    // 一律按这个尺寸夹取，绝不相信引擎传来的数值。
    uint32_t rt_w_ = 0;
    uint32_t rt_h_ = 0;

    // ---- 动态状态（逐 draw 命令下发，进 PSO 缓存键）----
    // D3D12 的深度/混合/剔除都编译进 PSO，所以状态变化 = 选另一条 PSO。
    // 这与 Vulkan 后端的 pipeline_cache_ 策略一致。
    bool depth_test_ = true;
    bool depth_write_ = true;
    bool blend_ = true;
    BlendFactor blend_src_ = BlendFactor::SrcAlpha;
    BlendFactor blend_dst_ = BlendFactor::OneMinusSrcAlpha;
    BlendEquation blend_equation_ = BlendEquation::Add;
    CullMode cull_ = CullMode::Back;
    // 当前渲染目标的格式（决定 PSO 的 RTVFormats/DSVFormat）。
    // PSO 把 RT 格式编译进去，所以换 RT 必须换 PSO —— 与 Vulkan 一致。
    DXGI_FORMAT current_rt_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT current_ds_format_ = DXGI_FORMAT_UNKNOWN;
    // 当前绑定 RT 颜色附件 0 的多样本数（决定 PSO 的 SampleDesc.Count 与
    // RasterizerState.MultisampleEnable；多样本 RT 由 RenderPipeline 显式开启）。
    uint32_t current_rt_samples_ = 1;

    // 适配器描述（api_version() 返回它；init 成功后有效）
    char adapter_desc_[160] = {};
    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_12_0;
};

} // namespace gryce_engine::render::d3d12
