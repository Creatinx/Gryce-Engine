#pragma once

#include <atomic>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLFramePacing — OpenGL/NVIDIA 特定的 GPU 帧率控制
// 支持 WGL_NV_delay_before_swap 扩展，可在 swap 前精确等待。
// 也提供 GPU busy-spin（glFinish 循环）选项，用于让 GPU 保持高占用。
// ---------------------------------------------------------------------------
class GLFramePacing {
public:
    GLFramePacing();

    // 检测并初始化扩展（必须在 GL context current 的线程调用）
    void init(void* native_window);

    // 是否支持 NVIDIA delay-before-swap
    bool supports_nv_delay_before_swap() const { return nv_delay_before_swap_supported_.load(); }

    // 设置 swap 前延迟（秒），用于精确控制帧率。仅在支持 NVIDIA 扩展时有效。
    // seconds <= 0 表示不延迟。
    void set_nv_delay_before_swap(float seconds);

    // 设置 GPU busy-spin 强度。0 表示关闭。
    void set_gpu_busy_iterations(int iterations) { gpu_busy_iterations_.store(iterations); }
    int gpu_busy_iterations() const { return gpu_busy_iterations_.load(); }

    void set_gpu_busy_enabled(bool enabled) { gpu_busy_enabled_.store(enabled); }
    bool gpu_busy_enabled() const { return gpu_busy_enabled_.load(); }

    // 在 swap buffers 之前调用（必须在渲染线程，GL context current）
    void before_swap();

private:
#ifdef _WIN32
    using PFNWGLDELAYBEFORESWAPNVPROC = BOOL (WINAPI*)(HDC hDC, float seconds);
#else
    using PFNWGLDELAYBEFORESWAPNVPROC = void*;
#endif

    std::atomic<bool> nv_delay_before_swap_supported_{false};
    std::atomic<float> nv_delay_seconds_{0.0f};
    std::atomic<bool> gpu_busy_enabled_{false};
    std::atomic<int> gpu_busy_iterations_{0};

    void* native_window_ = nullptr;
    PFNWGLDELAYBEFORESWAPNVPROC nv_delay_proc_ = nullptr;
};

} // namespace gryce_engine::render
