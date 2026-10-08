#pragma once

#include <thread>
#include <atomic>

#include "render/export.h"

namespace gryce_engine::render {

class IRenderBackend;
class RenderCommandBuffer;

// ---------------------------------------------------------------------------
// RenderThread — 渲染服务器线程
// 持有 GL context，消费 CMDBUFFER 中的命令并执行
// 预留扩展：可在内部启动更多子线程用于并行运算
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderThread {
public:
    RenderThread(IRenderBackend* backend, RenderCommandBuffer* cmd_buffer, void* native_window);
    ~RenderThread();

    void start();
    void stop();

    // 暂停渲染线程：优雅地结束当前帧并退出，不关闭 backend / command buffer，
    // 以便主线程重新接管 GL context 上传资源后再次 resume。
    void pause();

    void join();

    bool is_running() const;

    // 渲染线程已进入 thread_loop 并完成 context 绑定（可安全开始 submit）
    bool is_ready() const { return ready_.load(); }

    // 当前调用是否发生在渲染线程上。GPU 资源创建（纹理/FBO/SSBO）**必须**在
    // 渲染线程执行 —— GL context 只在渲染线程 current，主线程直接调
    // glCreateTextures/glCreateFramebuffers 会静默失败（返回 0）。
    // 外部在主线程要重建 GPU 资源时，必须先经此判断决定是否转发。
    bool on_render_thread() const {
        return running_.load() && thread_id_ == std::this_thread::get_id();
    }

private:
    void thread_loop();

    IRenderBackend* backend_;
    RenderCommandBuffer* cmd_buffer_;
    void* native_window_;

    std::thread thread_;
    std::atomic<std::thread::id> thread_id_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> shutdown_backend_on_exit_{true};
    std::atomic<bool> ready_{false};
};

} // namespace gryce_engine::render
