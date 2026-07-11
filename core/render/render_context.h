#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "export.h"
#include "render/render.h"
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
class GRYCE_API RenderContext {
public:
    // 用于外部对象（如 Renderer2D）安全检测 RenderContext 是否仍存活
    struct Lifetime {
        std::atomic<bool> alive{true};
    };

    RenderContext();
    ~RenderContext();

    // 初始化 backend（context 绑定到主线程）
    bool init(void* native_window, RenderAPI api = RenderAPI::OpenGL);

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

    // -----------------------------------------------------------------------
    // 资源创建（必须在 start() 之前调用，此时 context 仍在主线程）
    // -----------------------------------------------------------------------
    RHIMeshHandle create_mesh();
    RHIShaderHandle create_shader();
    RHITextureHandle create_texture();
    RHIFramebufferHandle create_framebuffer();

    // 资源销毁（push 到 CMDBUFFER，必须在 start() 之后调用）
    void destroy_mesh(RHIMeshHandle handle);
    void destroy_shader(RHIShaderHandle handle);
    void destroy_texture(RHITextureHandle handle);
    void destroy_framebuffer(RHIFramebufferHandle handle);

    // 通过句柄访问实际资源（必须在渲染线程使用）
    IMesh* mesh(RHIMeshHandle handle) const;
    IShader* shader(RHIShaderHandle handle) const;
    ITexture* texture(RHITextureHandle handle) const;
    IFramebuffer* framebuffer(RHIFramebufferHandle handle) const;

    // 渲染命令（必须在 start() 之后调用，push 到 CMDBUFFER）
    // -----------------------------------------------------------------------
    void set_shader(RHIShaderHandle shader);
    void set_uniform_int(RHIShaderHandle shader, const std::string& name, int value);
    void set_uniform_int(RHIShaderHandle shader, const char* name, int value);
    void set_uniform_float(RHIShaderHandle shader, const std::string& name, float value);
    void set_uniform_float(RHIShaderHandle shader, const char* name, float value);
    void set_uniform_vec3(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector3f& value);
    void set_uniform_vec3(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector3f& value);
    void set_uniform_mat4(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Matrix4f& value);
    void set_uniform_mat4(RHIShaderHandle shader, const char* name, const gryce_engine::math::Matrix4f& value);
    void set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                     const std::string& uniform_name = "uTexture");
    void set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                     const char* uniform_name = "uTexture");
    void clear(float r, float g, float b, float a);
    void clear_depth();
    void set_viewport(int x, int y, int w, int h);
    void set_depth_test(bool enabled);
    void set_blend(bool enabled);
    void set_cull_face(bool enabled);
    void set_framebuffer(RHIFramebufferHandle fb);
    void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader);
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader);

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
    void set_gpu_busy_spin(bool enabled, int iterations);
    void set_nv_delay_before_swap(float seconds);
    bool supports_nv_delay_before_swap() const;

    IRenderBackend* backend() const;

    // 高层便捷方法：委托给 backend 或在运行期推入命令缓冲
    void request_screenshot(const std::string& path);
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
    bool running_ = false;

    std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>();

    // 帧延迟销毁队列：命令缓冲中可能还有引用该资源的绘制命令，
    // 因此把真实删除推迟到 safe_seq 之前的所有帧都被渲染线程处理完后。
    std::vector<PendingDestroy> pending_destroys_;
    std::mutex pending_destroys_mutex_;

    uint64_t safe_destroy_seq() const;
    void enqueue_destroy(std::function<void()>&& deleter);
    void process_pending_destroys(bool force_all = false);
};

} // namespace gryce_engine::render
