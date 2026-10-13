#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "render/export.h"
#include "render/render.h"
#include "render/buffer.h"
#include "render/shader.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderCommandBuffer;
class RenderThread;
class IFramebuffer;

// ---------------------------------------------------------------------------
// RenderContext — 高层渲染上下文
// 组合 backend + CMDBUFFER + RenderThread
// 主线程通过 push 命令与渲染线程通信
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderContext {
public:
    // 用于外部对象（如 Renderer2D）安全检测 RenderContext 是否仍存活
    struct Lifetime {
        std::atomic<bool> alive{true};
    };

    RenderContext();
    ~RenderContext();

    // 初始化 backend（context 绑定到主线程）
    // 调用者通过 create_render_backend() 创建 backend 后传入
    bool init(void* native_window, std::unique_ptr<IRenderBackend> backend);

    // 返回一个与 RenderContext 生命周期绑定的标志，供外部对象弱引用
    std::shared_ptr<Lifetime> lifetime() const { return lifetime_; }

    // 启动渲染线程（释放主线程 context，绑定到渲染线程）
    // 调用后所有 GL 操作均在渲染线程执行
    void start();

    // 暂停渲染线程，把 GL context 切回主线程（用于热重载时上传 GPU 资源）
    void pause_render_thread();

    // 暂停渲染线程，但保留 cmd_buffer_ 用于继续 push / drain 清理命令
    void pause_render_thread_keep_cmdbuffer();

    // 恢复渲染线程（释放主线程 context，重新交给渲染线程）
    void resume_render_thread();

    // 关闭并等待渲染线程退出
    void shutdown();

    bool is_running() const;
    bool is_initialized() const;

    // 当前是否允许直接创建 GPU 资源（GL/Vulkan context 已 current）。
    // - 渲染线程运行时：只有渲染线程自己为 true
    // - 渲染线程未运行（启动前 / 已暂停 / 已关闭）：context 在主线程，为 true
    bool on_render_thread() const;

    // 在正确的上下文中执行 GPU 资源操作：已在渲染线程则直接跑，否则转发。
    // 返回 false 表示资源操作无法在当前状态下安全执行（渲染线程未运行）。
    // **凡是会 create/destroy 纹理、FBO、SSBO、pipeline 的代码都必须走这里**，
    // 直接在主线程调 GL 会静默失败（glCreateTextures 返回 0）。
    bool run_with_gpu_context(std::function<void()> fn);

    // 同上，但**阻塞到渲染线程执行完**才返回。
    // 适用于会同时改写跨线程共享句柄（hdr_color_ / hdr_depth_ / hdr_fbo_ 等）
    // 的重建操作：这些句柄同时被主线程的 render_scene 读取来绑定渲染目标，
    // 异步执行会让主线程读到半途状态 —— Vulkan 上表现为绑到未完成的 render
    // pass，主 pass 静默画不出任何东西（只剩走独立路径的粒子）。
    bool run_with_gpu_context_sync(std::function<void()> fn);

    // -----------------------------------------------------------------------
    // 资源创建（必须在 start() 之前调用，此时 context 仍在主线程）
    // -----------------------------------------------------------------------
    RHIMeshHandle create_mesh();
    RHIShaderHandle create_shader();
    RHITextureHandle create_texture();
    RHIFramebufferHandle create_framebuffer();
    RHIBufferHandle create_buffer();

    // 资源销毁（push 到 CMDBUFFER，必须在 start() 之后调用）
    void destroy_mesh(RHIMeshHandle handle);
    void destroy_shader(RHIShaderHandle handle);
    void destroy_texture(RHITextureHandle handle);
    void destroy_framebuffer(RHIFramebufferHandle handle);
    void destroy_buffer(RHIBufferHandle handle);

    // 通过句柄访问实际资源（必须在渲染线程使用）
    IMesh* mesh(RHIMeshHandle handle) const;
    IShader* shader(RHIShaderHandle handle) const;
    ITexture* texture(RHITextureHandle handle) const;
    IFramebuffer* framebuffer(RHIFramebufferHandle handle) const;
    IBuffer* buffer(RHIBufferHandle handle) const;

    // 渲染命令（必须在 start() 之后调用，push 到 CMDBUFFER）
    // -----------------------------------------------------------------------
    void set_shader(RHIShaderHandle shader);
    // uniform 名重载说明（性能相关，勿删）：
    // 同时提供 const std::string& 与 const char* 两个版本时，调用方写字符串字面量
    // （"uModel"）会优先匹配 std::string —— 数组到指针是标准转换，而字面量转
    // std::string 是用户定义转换，前者更优。结果是**每次调用都堆分配一个临时
    // std::string，只为立刻取 .c_str()**。逐物体路径（每帧上千次）上这是纯浪费。
    // 因此对所有"字面量必然走这里"的接口再加一个 const char(&)[N] 模板，
    // 它比 std::string 更精确匹配（恒等转换），直接转发到 const char* 版本。
    template <size_t N> void set_uniform_int(RHIShaderHandle shader, const char (&name)[N], int value) {
        set_uniform_int(shader, static_cast<const char*>(name), value);
    }
    void set_uniform_int(RHIShaderHandle shader, const std::string& name, int value);
    void set_uniform_int(RHIShaderHandle shader, const char* name, int value);
    template <size_t N> void set_uniform_float(RHIShaderHandle shader, const char (&name)[N], float value) {
        set_uniform_float(shader, static_cast<const char*>(name), value);
    }
    void set_uniform_float(RHIShaderHandle shader, const std::string& name, float value);
    void set_uniform_float(RHIShaderHandle shader, const char* name, float value);
    void set_uniform_vec2(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector2f& value);
    void set_uniform_vec2(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector2f& value);
    void set_uniform_vec3(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector3f& value);
    void set_uniform_vec3(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector3f& value);
    void set_uniform_vec4(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector4f& value);
    void set_uniform_vec4(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector4f& value);
    template <size_t N> void set_uniform_mat4(RHIShaderHandle shader, const char (&name)[N], const gryce_engine::math::Matrix4f& value) {
        set_uniform_mat4(shader, static_cast<const char*>(name), value);
    }
    void set_uniform_mat4(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Matrix4f& value);
    void set_uniform_mat4(RHIShaderHandle shader, const char* name, const gryce_engine::math::Matrix4f& value);
    // 后处理参数（uSSR* / uSSAO* / tonemap 等一整套）。★ 必须走本接口（命令
    // 缓冲）而不是拿 ctx->shader(h) 直接调 IShader::set_post_process_params：
    // D3D12 的 uniform 影子内存由渲染线程按帧复位，主线程录制期直接写会被
    // 复位擦掉 —— SSR 偶发整帧丢失（好帧/坏帧随机）正是这个竞争。
    void set_post_process_params(RHIShaderHandle shader, const PostProcessParams& params);
    // 同一份参数发给多个 shader 时用此重载：只拷贝一次 shared_ptr。
    void set_post_process_params(RHIShaderHandle shader,
                                 std::shared_ptr<const PostProcessParams> params);
    // mat4 数组 uniform（骨骼 palette）。shared_ptr 按值捕获进命令队列，
    // 渲染线程执行时数据仍有效；调用方随后可安全复用/释放自己的引用。
    void set_uniform_mat4_array(RHIShaderHandle shader, const char* name,
                                std::shared_ptr<const std::vector<gryce_engine::math::Matrix4f>> values);
    void set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                     const std::string& uniform_name = "uTexture");
    void set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                     const char* uniform_name = "uTexture");
    // 按"原始深度"绑定纹理（PCSS / SSAO 读真实深度；OpenGL 会绑非比较 sampler）
    void set_texture_raw_depth(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                               const std::string& uniform_name = "uTexture");
    void set_texture_raw_depth(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                               const char* uniform_name = "uTexture");
    void clear(float r, float g, float b, float a);
    void clear_depth();
    void set_viewport(int x, int y, int w, int h);
    void set_scissor(int x, int y, int w, int h);
    void set_depth_test(bool enabled);
    // 深度写入开关（默认开；渲染透明物体时关闭、渲染完恢复）
    void set_depth_write(bool enabled);
    void set_blend(bool enabled);
    // 混合因子（配合 set_blend(true) 使用；SSR 合成用 One/One 做加法混合）
    void set_blend_func(BlendFactor src, BlendFactor dst);
    void set_cull_face(CullMode mode);
    void set_framebuffer(RHIFramebufferHandle fb);
    // MSAA 解析（主 HDR 目标启用多样本时，场景 pass 结束后调用）。
    // 把 src（多样本）的颜色附件解析到 dst（单样本）；两端均非多样本时
    // 后端为空操作。与其它渲染命令一样走命令缓冲，在渲染线程按序执行。
    void resolve_framebuffer(RHIFramebufferHandle src, RHIFramebufferHandle dst);
    void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader);
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader);

    // -----------------------------------------------------------------------
    // 计算着色器 / 间接绘制（GPU-driven；不支持的后端上全部 no-op）
    // -----------------------------------------------------------------------
    // 派发计算工作组。shader 必须由 backend 的 create_compute_shader 创建。
    void dispatch_compute(RHIShaderHandle shader, uint32_t gx, uint32_t gy, uint32_t gz);
    // 按名字把 SSBO 绑到着色器声明的 layout(binding=N)。
    void bind_storage_buffer(RHIShaderHandle shader, const char* name, RHIBufferHandle buffer);
    // 指定后续间接绘制的每实例属性流（Vulkan 用它替代 mesh 的实例缓冲；
    // OpenGL 由顶点着色器直接读 SSBO，此为 no-op）。传无效句柄恢复默认。
    void bind_instance_stream(RHIBufferHandle buffer);
    // 间接绘制：绘制参数来自 GPU 缓冲（由 compute 写入）。
    void draw_indexed_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                               RHIBufferHandle args, uint32_t offset);
    void draw_arrays_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                              RHIBufferHandle args, uint32_t offset);
    // 在渲染线程上更新缓冲内容（data 会被拷贝进命令，调用方可立即释放）。
    void update_buffer(RHIBufferHandle buffer, const void* data, size_t size, size_t offset = 0);
    // 纹理 → 缓冲的 GPU 端拷贝（供 compute 读原本只有颜色附件形态的数据）。
    void copy_texture_to_buffer(RHITextureHandle texture, RHIBufferHandle buffer, uint32_t offset);
    // 显式关闭当前 render pass（Vulkan 用；使拷贝/compute 能在 pass 外录制，
    // 其余后端为 no-op）。调用后须重新 set_framebuffer 才能继续绘制。
    void end_render_pass_for_transfer();
    // 在渲染线程上（重新）创建缓冲。
    void create_buffer_storage(RHIBufferHandle buffer, size_t size, const void* data,
                               BufferUsage usage);

    // 提交自定义命令到 CMDBUFFER（用于扩展如 2D 渲染器）
    void push_command(std::function<void(IRenderBackend*)>&& cmd);

    // 提交当前帧命令，阻塞等待渲染线程完成
    void present();

    // 等待渲染线程处理完所有已提交帧（用于 frame pacing 或资源清理）
    void wait_for_idle();

    // -----------------------------------------------------------------------
    // 帧率 / 呈现控制（通过渲染线程执行）
    // -----------------------------------------------------------------------
    void set_swap_interval(int interval);

    // GPU 分段计时开关（渲染线程会读它，故走命令队列；dump 只在渲染线程空闲时
    // 由调用方使用，见 render_api.cpp 的实现）。
    void set_gpu_profiling(bool enabled);
    bool gpu_profiling_supported() const;
    IRenderBackend* gpu_profiling_backend() const { return backend_.get(); }

    // 把一段 GPU 计时区间排进命令流。
    // **必须走命令队列**：pipeline 在主线程跑、真正的 GL/VK 调用在渲染线程按
    // 命令顺序执行，只有在命令流里插入标记，时间戳才会落在正确的 pass 边界上。
    void gpu_profile_begin(const char* name);
    void gpu_profile_end();

    // 在渲染线程执行一段代码；同步模式（无渲染线程）直接执行。
    // 用途：GPU 资源创建（纹理/FBO）必须在持有上下文的线程上做，而 render_scene
    // 与 GViewport_SetSize 等入口跑在主线程 —— GL 在无 current context 的线程上
    // glCreateFramebuffers 会返回 0，表现为"某个分辨率下某个效果整体消失"。
    void run_on_render_thread(std::function<void()> fn);
    void set_gpu_busy_spin(bool enabled, int iterations);
    void set_nv_delay_before_swap(float seconds);
    bool supports_nv_delay_before_swap() const;

    IRenderBackend* backend() const;

    // 高层便捷方法：委托给 backend 或在运行期推入命令缓冲
    void request_screenshot(const std::string& path);

    // 调试探针透传（默认空实现，见 IRenderBackend::probe_texture）
    void probe_texture(RHITextureHandle tex, const char* tag);

    // 同步模式 present：提交、获取、执行、释放，全部在主线程内完成（无 RenderThread）
    void present_sync();

    // 同步模式拆分：execute_pending_sync() 只在当前线程执行已提交命令（不交换缓冲），
    // present_swap() 只做交换缓冲 + 处理延迟销毁。编辑器渲染线程用它把可能阻塞的
    // glfwSwapBuffers 移到 API 锁之外，避免 vsync 卡死时冻结 UI 线程。
    void execute_pending_sync();
    void present_swap();

    // 同步读取当前帧为 top-down RGBA。阻塞直到渲染线程完成本帧。

    // 同步读取当前帧为 top-down RGBA。阻塞直到渲染线程完成本帧。
    bool capture_frame_rgba(std::vector<uint8_t>& out, int& w, int& h);

    std::unique_ptr<IRenderer2D> create_renderer2d();
    std::unique_ptr<IImGuiBackend> create_imgui_backend();
    void set_validation_enabled(bool enabled); // must be called before init()

private:
    struct PendingDestroy {
        std::function<void()> deleter;
        uint64_t safe_seq;
    };

    std::unique_ptr<IRenderBackend> backend_;
    std::unique_ptr<RenderCommandBuffer> cmd_buffer_;
    std::unique_ptr<RenderThread> render_thread_;
    void* native_window_ = nullptr;
    bool initialized_ = false;
    // Vulkan 校验层默认关闭：它会拦截每一条 vkCmd* 调用，把命令录制拖慢数倍，
    // 帧时间被 CPU 侧吃满（与 vk_instance.h 的"默认关闭以提升性能"一致）。
    // 排查校验报错时用环境变量 GRYCE_VULKAN_VALIDATION=1 显式打开。
    bool validation_enabled_ = false;
    bool running_ = false;

    // 同步模式（无渲染线程）下 destroy_* 直接调用 backend；为避免销毁仍被
    // 上一帧 in-flight 命令缓冲引用的 Vulkan 资源，记录最近一次提交序号，
    // 销毁前若有过新提交则先 wait_gpu_idle（每次提交最多等待一次）。
    uint64_t sync_submit_seq_ = 0;
    uint64_t sync_waited_seq_ = 0;

    std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>();

    // 本帧通过 set_texture 写过的采样器 uniform：(shader, uniform) 组合。
    // 用于拦掉随后重复的 set_uniform_int(name, slot)，避免把后端换算后的
    // 纹理单元覆盖回原始槽位（槽位超过单元上限时采样恒为 0）。
    std::vector<std::pair<RHIShaderHandle, std::string>> texture_uniform_writes_;
    bool is_texture_uniform(RHIShaderHandle shader, const char* name) const;

    // 帧延迟销毁队列：命令缓冲中可能还有引用该资源的绘制命令，
    // 因此把真实删除推迟到 safe_seq 之前的所有帧都被渲染线程处理完后。
    std::vector<PendingDestroy> pending_destroys_;
    std::mutex pending_destroys_mutex_;

    uint64_t safe_destroy_seq() const;
    void enqueue_destroy(std::function<void()>&& deleter);
    void process_pending_destroys(bool force_all = false);

    // GPU 计时是否开启（主线程写、流水线读；关闭时 gpu_profile_* 不再产生命令）。
    std::atomic<bool> gpu_profiling_enabled_{false};
};

} // namespace gryce_engine::render
