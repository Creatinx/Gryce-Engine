// =============================================================================
// d3d12_backend — D3D12 后端
//
// 已实现范围（M2 + M3）：
//   * M2：init（loader → factory → 适配器 → 设备 → 队列 → 交换链）、
//     begin/end_frame 的真实录制-提交-Present 循环、clear、set_viewport、
//     shutdown、wait_gpu_idle、set_swap_interval。
//   * M3：资源层 —— 描述符堆管理器（CBV_SRV_UAV + Sampler）、上传路径
//     （staging / UploadRing）、Buffer、Texture（含 mip / cubemap /
//     压缩 / HDR）、Mesh、Framebuffer，以及五个 RHIResourcePool 的接线。
//
// 尚未实现（M4+，见 docs/d3d12-backend-plan.md）：
//   着色器链（GLSL→SPIR-V→HLSL→DXIL）、管线状态对象（root signature + PSO）、
//   2D 渲染器、ImGui 后端、RenderPipeline 端到端联调。
//   这些接口仍返回空句柄 / nullptr，调用方按 IRenderBackend 默认语义回退。
// =============================================================================
#pragma once

// D3D12 与 DXGI 头。IDXGIFactory4 在 dxgi1_4.h（不是 d3d12.h 或 dxgi.h），
// 缺了它会在 GCC 下报 "'IDXGIFactory4' does not name a type"。
// mingw-w64 vendor 头与 Windows SDK 头都提供这两个文件。
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>

#include "render/render.h"
#include "render/rhi_resource_pool.h"
#include "d3d12_loader.h"
#include "d3d12_swapchain.h"
#include "d3d12_descriptors.h"
#include "d3d12_upload.h"
#include "d3d12_buffer.h"
#include "d3d12_texture.h"
#include "d3d12_mesh.h"
#include "d3d12_framebuffer.h"
#include "d3d12_shader.h"

namespace gryce_engine::render::d3d12 {

class D3D12Backend final : public IRenderBackend {
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
    void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) override;
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) override;

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
    // 把当前帧的 clear 录进命令列表（含 back buffer 状态转换）。
    void record_clear_if_dirty();
    // 把 RT 切回交换链 back buffer（idempotent：已是当前 RT 时不做重复屏障）。
    // bind_framebuffer 解析失败时也走这里 —— 见 GL 的同名语义。
    void bind_swapchain_rt();
    // 输出 D3D12 调试层的校验消息（仅 GRYCE_D3D12_DEBUG=1 时有内容）。
    void dump_info_queue();
    // 把回读的 RGBA 写成 PNG/BMP 落盘。
    void save_screenshot(const std::string& path);
    // native_window 是 GLFWwindow*（见 render_context.cpp:157），D3D12 需要真实
    // HWND —— 走 glfwGetWin32Window 转换。非 GLFW 句柄时返回 nullptr。
    static HWND hwnd_from_native(void* native_window);

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

    // 当前绑定的帧缓冲（nullptr 表示渲染到交换链 back buffer）。
    RHIFramebufferHandle bound_framebuffer_{};

    HWND hwnd_ = nullptr;
    bool initialized_ = false;
    bool frame_aborted_ = false;
    bool device_lost_ = false;
    bool debug_layer_enabled_ = false;
    // 单调递增的帧号：shader 用它做惰性"每帧重置"，触发点在**本帧第一次写
    // shader 状态时**（不是 draw 时 —— 见 current_frame_id() 的时序说明）。
    uint64_t frame_counter_ = 0;
    // 本帧 back buffer 是否已被切到 RENDER_TARGET（决定 end_frame 是否要切回 PRESENT）
    bool back_buffer_as_rt_ = false;

    // ---- 截图（M5.3）----
    // 与 GL/Vulkan 同构：request_screenshot 记下路径与目标帧号，end_frame 里
    // 帧号一到就回读 back buffer 并落盘。跨帧号是必要的 —— 请求发生在帧中间，
    // 立刻回读拿到的是上一帧的内容。
    std::string screenshot_path_;
    uint64_t screenshot_frame_ = 0;

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

    // 适配器描述（api_version() 返回它；init 成功后有效）
    char adapter_desc_[160] = {};
    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_12_0;
};

} // namespace gryce_engine::render::d3d12
