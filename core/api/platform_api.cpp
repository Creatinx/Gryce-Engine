#include "GrycePlatform/window_api.h"
#include "GryceCore/api_guard.h"
#include "GrycePlatform/input_api.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#include "platform/window.h"
#include "platform/input.h"
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <memory>
#include <mutex>

using gryce_engine::platform::Window;
using gryce_engine::platform::InputManager;
using gryce_engine::platform::WindowMode;
using gryce_engine::platform::WindowContextType;

namespace {

// ---------------------------------------------------------------------------
// Platform state — supports both GLFW and External HWND modes
// ---------------------------------------------------------------------------
struct PlatformState {
    enum class Mode { None, GLFW, External };
    Mode mode = Mode::None;

    // GLFW mode
    std::unique_ptr<Window> window;
    std::unique_ptr<InputManager> input_mgr;

    // External HWND mode
    void* external_hwnd = nullptr;
    int ext_width = 0;
    int ext_height = 0;
    GLFWwindow* embedded_window = nullptr;

    // Shared input state (updated by injection or GLFW polling)
    struct InputState {
        bool keys[512] = {};
        bool keys_prev[512] = {};
        bool mouse_buttons[8] = {};
        bool mouse_buttons_prev[8] = {};
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        float mouse_delta_x = 0.0f;
        float mouse_delta_y = 0.0f;
        bool first_mouse = true;
    } input;

    std::mutex input_mutex;
};

static PlatformState g_platform;

// GLFW 错误默认是静默的；注册回调把上下文创建/窗口错误暴露到日志，
// 方便编辑器 Console 面板直接看到底层失败原因。
static void glfw_error_callback(int code, const char* desc) {
    GLOG_ERROR("GLFW error {}: {}", code, desc ? desc : "(no description)");
}

static void copy_input_prev_to_current() {
    std::memcpy(g_platform.input.keys_prev, g_platform.input.keys, sizeof(g_platform.input.keys));
    std::memcpy(g_platform.input.mouse_buttons_prev, g_platform.input.mouse_buttons, sizeof(g_platform.input.mouse_buttons));
}

static void update_input_from_window() {
    if (!g_platform.window || !g_platform.window->is_valid()) return;

    copy_input_prev_to_current();

    for (int i = 0; i < 512; ++i) {
        g_platform.input.keys[i] = g_platform.window->get_key(i);
    }

    GLFWwindow* handle = static_cast<GLFWwindow*>(g_platform.window->native_handle());
    for (int i = 0; i < 8; ++i) {
        g_platform.input.mouse_buttons[i] = (glfwGetMouseButton(handle, i) == GLFW_PRESS);
    }

    double x, y;
    g_platform.window->get_cursor_pos(x, y);
    g_platform.input.mouse_x = static_cast<float>(x);
    g_platform.input.mouse_y = static_cast<float>(y);

    if (g_platform.input.first_mouse) {
        g_platform.input.mouse_delta_x = 0.0f;
        g_platform.input.mouse_delta_y = 0.0f;
        g_platform.input.first_mouse = false;
    } else {
        g_platform.input.mouse_delta_x = g_platform.input.mouse_x - g_platform.input.mouse_x;
        g_platform.input.mouse_delta_y = g_platform.input.mouse_y - g_platform.input.mouse_y;
    }
}

static WindowMode to_internal_mode(GWindowMode mode) {
    switch (mode) {
        case GWINDOW_MODE_FULLSCREEN: return WindowMode::Fullscreen;
        case GWINDOW_MODE_BORDERLESS: return WindowMode::Borderless;
        default: return WindowMode::Windowed;
    }
}

} // namespace

extern "C" {

// ========== Window ==========

int GWindow_InitExternal(GWindowHandle hwnd, int w, int h) {
    GRYCE_API_GUARD();
    if (!hwnd) return -1;
    if (g_platform.mode != PlatformState::Mode::None) {
        GLOG_WARN("GWindow_InitExternal: platform already initialized, shutting down first");
        GWindow_Destroy();
    }

    if (!Window::init_sdk()) {
        GLOG_ERROR("GWindow_InitExternal: GLFW init failed");
        return -1;
    }

    glfwSetErrorCallback(glfw_error_callback);

    // 为外部 HWND 创建一个隐藏的 GLFW 子窗口，用于提供 OpenGL 上下文并实际渲染。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* embedded = glfwCreateWindow(w > 0 ? w : 640, h > 0 ? h : 480,
                                            "GryceEditorViewport", nullptr, nullptr);
    if (!embedded) {
        GLOG_ERROR("GWindow_InitExternal: failed to create embedded GLFW window");
        Window::shutdown_sdk();
        return -1;
    }

    // 诊断：确认嵌入窗口是否真的带有 OpenGL 上下文（GLFW 默认创建）。
    GLOG_INFO("Embedded window client_api={} ctx={}.{}",
              glfwGetWindowAttrib(embedded, GLFW_CLIENT_API),
              glfwGetWindowAttrib(embedded, GLFW_CONTEXT_VERSION_MAJOR),
              glfwGetWindowAttrib(embedded, GLFW_CONTEXT_VERSION_MINOR));

#ifdef _WIN32
    HWND embedded_hwnd = glfwGetWin32Window(embedded);
    if (embedded_hwnd) {
        SetParent(embedded_hwnd, static_cast<HWND>(hwnd));
        LONG style = GetWindowLong(embedded_hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                   WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
        style |= WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        SetWindowLong(embedded_hwnd, GWL_STYLE, style);
        SetWindowPos(embedded_hwnd, nullptr, 0, 0,
                     w > 0 ? w : 640, h > 0 ? h : 480,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
#endif

    g_platform.mode = PlatformState::Mode::External;
    g_platform.external_hwnd = hwnd;
    g_platform.ext_width = w;
    g_platform.ext_height = h;
    g_platform.embedded_window = embedded;
    GLOG_INFO("Platform: external HWND initialized ({}x{}) with embedded GLFW window", w, h);
    return 0;
}

int GWindow_Create(const char* title, int w, int h, GWindowMode mode) {
    GRYCE_API_GUARD();
    if (g_platform.mode != PlatformState::Mode::None) {
        GLOG_WARN("GWindow_Create: platform already initialized, shutting down first");
        GWindow_Destroy();
    }

    if (!Window::init_sdk()) {
        GLOG_ERROR("GWindow_Create: GLFW init failed");
        return -1;
    }

    glfwSetErrorCallback(glfw_error_callback);

    g_platform.window = std::make_unique<Window>(
        title ? title : "Gryce Engine", w, h, to_internal_mode(mode));
    if (!g_platform.window->is_valid()) {
        GLOG_ERROR("GWindow_Create: failed to create window");
        Window::shutdown_sdk();
        return -1;
    }

    g_platform.input_mgr = std::make_unique<InputManager>();
    g_platform.mode = PlatformState::Mode::GLFW;
    GLOG_INFO("Platform: GLFW window created '{}' ({}x{})", title ? title : "Gryce Engine", w, h);
    return 0;
}

void GWindow_Destroy(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW) {
        if (g_platform.window) {
            g_platform.window->destroy();
            g_platform.window.reset();
        }
        g_platform.input_mgr.reset();
        Window::shutdown_sdk();
    } else if (g_platform.mode == PlatformState::Mode::External) {
        if (g_platform.embedded_window) {
            glfwDestroyWindow(g_platform.embedded_window);
            g_platform.embedded_window = nullptr;
        }
        Window::shutdown_sdk();
    }
    g_platform.mode = PlatformState::Mode::None;
    g_platform.external_hwnd = nullptr;
    g_platform.ext_width = 0;
    g_platform.ext_height = 0;
    std::memset(&g_platform.input, 0, sizeof(g_platform.input));
}

bool GWindow_IsValid(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW) {
        return g_platform.window && g_platform.window->is_valid();
    }
    if (g_platform.mode == PlatformState::Mode::External) {
        return g_platform.external_hwnd != nullptr;
    }
    return false;
}

void GWindow_GetSize(int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (!out_w || !out_h) return;
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        g_platform.window->get_size(*out_w, *out_h);
    } else if (g_platform.mode == PlatformState::Mode::External) {
        if (g_platform.embedded_window) {
            glfwGetWindowSize(g_platform.embedded_window, out_w, out_h);
        } else {
            *out_w = g_platform.ext_width;
            *out_h = g_platform.ext_height;
        }
    } else {
        *out_w = 0;
        *out_h = 0;
    }
}

void GWindow_SetSize(int w, int h) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        g_platform.window->set_size(w, h);
    } else if (g_platform.mode == PlatformState::Mode::External) {
        g_platform.ext_width = w;
        g_platform.ext_height = h;
        if (g_platform.embedded_window) {
            glfwSetWindowSize(g_platform.embedded_window, w, h);
#ifdef _WIN32
            HWND embedded_hwnd = glfwGetWin32Window(g_platform.embedded_window);
            if (embedded_hwnd) {
                SetWindowPos(embedded_hwnd, nullptr, 0, 0, w, h,
                             SWP_FRAMECHANGED | SWP_NOZORDER);
            }
#endif
        }
    }
}

GWindowHandle GWindow_GetNativeHandle(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        return g_platform.window->native_handle();
    }
    if (g_platform.mode == PlatformState::Mode::External) {
        return g_platform.external_hwnd;
    }
    return nullptr;
}

GWindowHandle GWindow_GetRenderHandle(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        return g_platform.window->native_handle();
    }
    if (g_platform.mode == PlatformState::Mode::External) {
        return g_platform.embedded_window;
    }
    return nullptr;
}

bool GWindow_ShouldClose(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        return g_platform.window->should_close();
    }
    return true;
}

void GWindow_PollEvents(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW) {
        glfwPollEvents();
        if (g_platform.input_mgr && g_platform.window) {
            g_platform.input_mgr->update(g_platform.window.get());
        }
        update_input_from_window();
    }
    // External mode: events are injected via GInput_Inject*, no polling needed
}

void GWindow_SwapBuffers(void) {
    GRYCE_API_GUARD();
    if (g_platform.mode == PlatformState::Mode::GLFW && g_platform.window) {
        g_platform.window->swap_buffers();
    }
    // External mode: swap is handled by Renderer
}

// ========== Input ==========

void GInput_InjectKey(int key_code, GInputAction action) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (key_code < 0 || key_code >= 512) return;
    g_platform.input.keys[key_code] = (action == GINPUT_ACTION_PRESS || action == GINPUT_ACTION_REPEAT);
}

void GInput_InjectMouseMove(float x, float y) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (g_platform.input.first_mouse) {
        g_platform.input.mouse_delta_x = 0.0f;
        g_platform.input.mouse_delta_y = 0.0f;
        g_platform.input.first_mouse = false;
    } else {
        g_platform.input.mouse_delta_x = x - g_platform.input.mouse_x;
        g_platform.input.mouse_delta_y = y - g_platform.input.mouse_y;
    }
    g_platform.input.mouse_x = x;
    g_platform.input.mouse_y = y;
}

void GInput_InjectMouseButton(int button, GInputAction action, float x, float y) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (button < 0 || button >= 8) return;
    g_platform.input.mouse_buttons[button] = (action == GINPUT_ACTION_PRESS || action == GINPUT_ACTION_REPEAT);
    g_platform.input.mouse_x = x;
    g_platform.input.mouse_y = y;
}

void GInput_InjectMouseScroll(float delta_x, float delta_y) {
    GRYCE_API_GUARD();
    // Scroll is event-based; for now, store in a separate field if needed
    (void)delta_x; (void)delta_y;
}

bool GInput_IsKeyPressed(int key_code) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (key_code < 0 || key_code >= 512) return false;
    return g_platform.input.keys[key_code] && !g_platform.input.keys_prev[key_code];
}

bool GInput_IsKeyHeld(int key_code) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (key_code < 0 || key_code >= 512) return false;
    return g_platform.input.keys[key_code];
}

bool GInput_IsMouseButtonPressed(int button) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (button < 0 || button >= 8) return false;
    return g_platform.input.mouse_buttons[button] && !g_platform.input.mouse_buttons_prev[button];
}

void GInput_GetMousePosition(float* out_x, float* out_y) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_platform.input_mutex);
    if (out_x) *out_x = g_platform.input.mouse_x;
    if (out_y) *out_y = g_platform.input.mouse_y;
}

} // extern "C"
