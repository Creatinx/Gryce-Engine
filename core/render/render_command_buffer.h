#pragma once

#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <variant>

#include "export.h"
#include "render/render_commands.h"

namespace gryce_engine::render {

class IRenderBackend;

// ---------------------------------------------------------------------------
// 单条渲染命令：封装一个对后端的无参调用（保留扩展入口）
// ---------------------------------------------------------------------------
using RenderCommand = std::function<void(IRenderBackend*)>;

// ---------------------------------------------------------------------------
// RenderCommandItem —  either 结构化命令 or 自定义 lambda
// ---------------------------------------------------------------------------
struct RenderCommandItem {
    std::variant<RenderCommandTyped, RenderCommand> data;

    RenderCommandItem() = default;
    RenderCommandItem(RenderCommandTyped typed) : data(std::move(typed)) {}
    RenderCommandItem(RenderCommand lambda) : data(std::move(lambda)) {}

    bool is_typed() const { return std::holds_alternative<RenderCommandTyped>(data); }
    const RenderCommandTyped& typed() const { return std::get<RenderCommandTyped>(data); }
    const RenderCommand& lambda() const { return std::get<RenderCommand>(data); }
};

// ---------------------------------------------------------------------------
// RenderCommandBuffer — CMDBUFFER
// 三缓冲命令队列，主线程写入 / 渲染线程读取，present 非阻塞（除非 3 帧都在 GPU）
//
// 状态机：
//   Free      -> 主线程可写入
//   Submitted -> 已提交，等待渲染线程处理
//   Rendering -> 渲染线程正在处理
//   Free      <- 渲染线程处理完成，归还主线程
//
// 三缓冲保证：主线程可以准备 N+1 帧，渲染线程处理 N 帧，同时有一帧 pending。
// ---------------------------------------------------------------------------
class GRYCE_API RenderCommandBuffer {
public:
    static constexpr int kFrameCount = 3;

    RenderCommandBuffer();
    ~RenderCommandBuffer();

    // 主线程：push 一条异步命令到当前帧 buffer
    void push(RenderCommand&& cmd);

    // 主线程：push 一条结构化命令到当前帧 buffer
    void push_typed(RenderCommandTyped&& cmd);

    // 主线程：提交当前帧。非阻塞；只有当全部 3 个 buffer 都被 GPU 占用时才会等待。
    void submit();

    // 渲染线程：获取当前待渲染的命令列表（按提交顺序）
    std::vector<RenderCommandItem>* acquire();

    // 渲染线程：执行完成后释放
    void release();

    // 请求关闭（唤醒所有等待线程）
    void shutdown();

    // 取出当前 write buffer 中尚未提交的命令（用于 RenderContext::shutdown 时 drain）
    std::vector<RenderCommandItem> drain();

    bool is_shutdown() const;

    // 等待所有已提交/渲染中的帧完成（变为 Free）
    void wait_for_idle();

    // 当前写入帧在下次 submit 时将获得的序列号
    uint64_t current_write_seq() const;

    // 渲染线程已完成的最高序列号
    uint64_t completed_seq() const;

private:
    struct Frame {
        std::vector<RenderCommandItem> commands;
        enum State { Free, Submitted, Rendering } state = Free;
        uint64_t seq = 0;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_main_;   // 主线程等待：有空闲 buffer
    std::condition_variable cv_render_; // 渲染线程等待：有已提交 frame

    Frame frames_[kFrameCount];
    int write_index_ = 0;
    uint64_t next_seq_ = 1;
    uint64_t completed_seq_ = 0;
    bool shutdown_ = false;
};

} // namespace gryce_engine::render
