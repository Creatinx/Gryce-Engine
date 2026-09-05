#include "gl_frame_pacing.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

GLFramePacing::GLFramePacing() = default;

void GLFramePacing::init(void* native_window) {
    native_window_ = native_window;

#ifdef _WIN32
    if (!native_window_) return;

    // 直接获取 WGL_NV_delay_before_swap 函数指针；非空即支持。
    // 避免依赖 wglGetExtensionsStringARB，减少头文件/加载器兼容问题。
    HDC hdc = GetDC(static_cast<HWND>(native_window_));
    if (hdc) {
        PFNWGLDELAYBEFORESWAPNVPROC proc = reinterpret_cast<PFNWGLDELAYBEFORESWAPNVPROC>(wglGetProcAddress("wglDelayBeforeSwapNV"));
        if (proc) {
            nv_delay_proc_ = proc;
            nv_delay_before_swap_supported_.store(true);
            GLOG_INFO("GLFramePacing: WGL_NV_delay_before_swap supported");
        }
        ReleaseDC(static_cast<HWND>(native_window_), hdc);
    }
#endif

    if (!nv_delay_before_swap_supported_.load()) {
        GLOG_INFO("GLFramePacing: WGL_NV_delay_before_swap not available");
    }
}

void GLFramePacing::set_nv_delay_before_swap(float seconds) {
    nv_delay_seconds_.store(seconds > 0.0f ? seconds : 0.0f);
}

void GLFramePacing::before_swap() {
#ifdef _WIN32
    // NVIDIA delay-before-swap：在 swap 前精确等待
    if (nv_delay_before_swap_supported_.load() && nv_delay_seconds_.load() > 0.0f) {
        PFNWGLDELAYBEFORESWAPNVPROC proc = nv_delay_proc_;
        if (proc && native_window_) {
            HWND hwnd = static_cast<HWND>(native_window_);
            HDC hdc = GetDC(hwnd);
            if (hdc) {
                proc(hdc, nv_delay_seconds_.load());
                ReleaseDC(hwnd, hdc);
            }
        }
    }
#endif

    // GPU busy-spin：通过 glFinish 循环让 GPU 保持忙碌
    if (gpu_busy_enabled_.load()) {
        int iterations = gpu_busy_iterations_.load();
        for (int i = 0; i < iterations; ++i) {
            glFinish();
        }
    }
}

} // namespace gryce_engine::render
