#pragma once

#include <thread>
#include <atomic>

#include "export.h"

namespace gryce_engine::render {

class IRenderBackend;
class RenderCommandBuffer;

// ---------------------------------------------------------------------------
// RenderThread — 渲染服务器线程
// 持有 GL context，消费 CMDBUFFER 中的命令并执行
// 预留扩展：可在内部启动更多子线程用于并行运算
// ---------------------------------------------------------------------------
class GRYCE_API RenderThread {
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

private:
    void thread_loop();

    IRenderBackend* backend_;
    RenderCommandBuffer* cmd_buffer_;
    void* native_window_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> shutdown_backend_on_exit_{true};
};

} // namespace gryce_engine::render
