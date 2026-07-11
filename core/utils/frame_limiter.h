#pragma once

#include <chrono>
#include <atomic>

namespace gryce_engine::utils {

// ---------------------------------------------------------------------------
// FrameLimiter — CPU 侧帧率限制器
// 提供高精度目标帧时间控制，支持 0 表示无上限。
// ---------------------------------------------------------------------------
class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();

    // 设置目标 FPS。0 表示不限制。
    void set_target_fps(int fps);
    int target_fps() const { return target_fps_.load(); }

    // 是否启用限制
    void set_enabled(bool enabled) { enabled_.store(enabled); }
    bool enabled() const { return enabled_.load(); }

    // 每帧开始时调用
    void begin_frame();

    // 每帧结束时调用；如果需要等待，会执行高精度 sleep 并返回实际等待时间（秒）。
    // 返回 0 表示无需等待。
    double end_frame();

    // 上一帧实际耗时（秒）
    double last_frame_time() const { return last_frame_time_.load(); }

    // 目标帧时间（秒），0 表示无限制
    double target_frame_time() const { return target_frame_time_.load(); }

private:
    std::atomic<int> target_fps_{0};
    std::atomic<bool> enabled_{true};
    std::atomic<double> target_frame_time_{0.0};
    std::atomic<double> last_frame_time_{0.0};

    std::chrono::steady_clock::time_point frame_start_;

#ifdef _WIN32
    // 高精度可等待定时器（CREATE_WAITABLE_TIMER_HIGH_RESOLUTION）
    void* timer_handle_ = nullptr;
    bool timer_high_res_ = false;
#endif
};

} // namespace gryce_engine::utils
