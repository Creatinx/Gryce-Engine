#include "window.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#include <GLFW/glfw3.h>

#include "cursor.h"
#ifdef _WIN32
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#include "utils/glog/glog_lib.h"

namespace gryce_engine::platform {

#ifdef _WIN32

// 无边框窗口：在边缘 6px 内返回系统缩放 hit-test 码，让用户能拖边调整大小。
// 最大化时禁用（最大化窗口占满工作区，边缘无意义）。
// 其余消息全部转发给原始 WndProc（GLFW 的 windowProc）。
static LRESULT CALLBACK window_resize_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = static_cast<Window*>(reinterpret_cast<void*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    if (msg == WM_NCHITTEST && self) {
        if (IsZoomed(hwnd)) {
            return HTCLIENT;
        }
        constexpr int kBorder = 6;
        POINT pt;
        pt.x = static_cast<short>(LOWORD(lParam));
        pt.y = static_cast<short>(HIWORD(lParam));
        RECT rc;
        GetWindowRect(hwnd, &rc);
        const bool on_left = pt.x - rc.left < kBorder;
        const bool on_right = rc.right - pt.x < kBorder;
        const bool on_top = pt.y - rc.top < kBorder;
        const bool on_bottom = rc.bottom - pt.y < kBorder;
        if (on_left && on_top) return HTTOPLEFT;
        if (on_right && on_top) return HTTOPRIGHT;
        if (on_left && on_bottom) return HTBOTTOMLEFT;
        if (on_right && on_bottom) return HTBOTTOMRIGHT;
        if (on_top) return HTTOP;
        if (on_bottom) return HTBOTTOM;
        if (on_left) return HTLEFT;
        if (on_right) return HTRIGHT;
        return HTCLIENT;
    }
    // 转发给原始 WndProc（GLFW 的 windowProc）
    if (self && self->native_wndproc()) {
        return CallWindowProcW(reinterpret_cast<WNDPROC>(self->native_wndproc()), hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// GLFW SDK 管理
// ---------------------------------------------------------------------------
bool Window::init_sdk() {
    int result = glfwInit();
    if (!result) {
        GLOG_ERROR("glfwInit() failed");
        return false;
    }
    GLOG_INFO("GLFW initialized successfully");
    return true;
}

void Window::shutdown_sdk() {
    glfwTerminate();
    GLOG_INFO("GLFW terminated");
}

// ---------------------------------------------------------------------------
// 外部辅助函数：获取 monitor / video mode
// ---------------------------------------------------------------------------
static GLFWmonitor* get_primary_monitor() {
    return glfwGetPrimaryMonitor();
}

static const GLFWvidmode* get_video_mode(GLFWmonitor* monitor) {
    return glfwGetVideoMode(monitor);
}

// ---------------------------------------------------------------------------
// 外部函数：根据模式创建 GLFW 原生窗口
// ---------------------------------------------------------------------------
GLFWwindow* Window::create_glfw_window(const std::string& title, int w, int h, WindowMode mode,
                                       WindowContextType context_type) {
    // Vulkan / NoAPI 模式必须禁用 GLFW 的默认 OpenGL context，否则 surface 创建会失败
    if (context_type == WindowContextType::NoApi) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }

    GLFWmonitor* monitor = nullptr;
    int win_w = w;
    int win_h = h;

    switch (mode) {
        case WindowMode::Windowed:
            monitor = nullptr;
            break;

        case WindowMode::Borderless: {
            monitor = get_primary_monitor();
            const GLFWvidmode* vm = get_video_mode(monitor);
            if (vm) {
                win_w = vm->width;
                win_h = vm->height;
            }
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
            break;
        }

        case WindowMode::Fullscreen: {
            monitor = get_primary_monitor();
            const GLFWvidmode* vm = get_video_mode(monitor);
            if (vm) {
                win_w = vm->width;
                win_h = vm->height;
            }
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            break;
        }

        case WindowMode::Maximized:
            glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
            break;
    }

    GLFWwindow* window = glfwCreateWindow(win_w, win_h, title.c_str(), monitor, nullptr);

    glfwDefaultWindowHints();
    return window;
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Window::Window(const std::string& title, int width, int height, WindowMode mode,
               WindowContextType context_type)
    : handle_(nullptr)
    , mode_(mode)
    , context_type_(context_type)
    , title_(title)
    , width_(width)
    , height_(height)
    , fps_(0.0)
    , delta_time_(0.0)
    , last_time_(0.0)
    , frame_count_(0)
    , fps_interval_(0.0) {
    handle_ = create_glfw_window(title, width, height, mode, context_type_);
    if (!handle_) {
        GLOG_ERROR("Failed to create window: title='{}', size=({}x{}), mode={}",
                   title, width, height, static_cast<int>(mode));
        return;
    }
    if (context_type_ == WindowContextType::OpenGL) {
        glfwMakeContextCurrent(handle_);
    }

    // 设置用户指针与回调
    glfwSetWindowUserPointer(handle_, this);
    glfwSetFramebufferSizeCallback(handle_, [](GLFWwindow* w, int wdt, int hgt) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (!self) return;
        self->width_ = wdt;
        self->height_ = hgt;
        if (self->resize_callback_) {
            self->resize_callback_(wdt, hgt);
        }
    });
    glfwSetWindowCloseCallback(handle_, [](GLFWwindow* w) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (!self) return;
        self->close_requested_ = true;
    });

    // Windows: 注册 Raw Input 键盘设备，RIDEV_NOHOTKEYS 阻止 Shift 切换输入法
#ifdef _WIN32
    if (HWND hwnd = glfwGetWin32Window(handle_)) {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;    // Generic Desktop
        rid.usUsage = 0x06;        // Keyboard
        rid.dwFlags = RIDEV_NOHOTKEYS;
        rid.hwndTarget = hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            GLOG_WARN("RegisterRawInputDevices(RIDEV_NOHOTKEYS) failed: {}", GetLastError());
        } else {
            GLOG_INFO("Raw Input keyboard registered (NOHOTKEYS)");
        }
    }
#endif

    GLOG_INFO("Window created: '{}' ({}x{}), mode={}",
              title, width, height, static_cast<int>(mode));

    // 无边框窗口边缘缩放边框（Win32 WndProc 子类化）。
    // GLFW 无边框窗口用 WS_POPUP（无 WS_THICKFRAME），不装这个就没有拖边缩放。
    install_resize_border();
}

Window::~Window() {
    destroy();
}

void Window::destroy() {
    if (handle_) {
        remove_resize_border();
        glfwDestroyWindow(handle_);
        GLOG_INFO("Window destroyed: '{}'", title_);
        handle_ = nullptr;
    }
}

Window::Window(Window&& o) noexcept
    : handle_(o.handle_)
    , mode_(o.mode_)
    , title_(std::move(o.title_))
    , width_(o.width_)
    , height_(o.height_)
    , fps_(o.fps_)
    , delta_time_(o.delta_time_)
    , last_time_(o.last_time_)
    , frame_count_(o.frame_count_)
    , fps_interval_(o.fps_interval_) {
    o.handle_ = nullptr;
#ifdef _WIN32
    original_wndproc_ = o.original_wndproc_;
    win32_hwnd_ = o.win32_hwnd_;
    o.original_wndproc_ = nullptr;
    o.win32_hwnd_ = nullptr;
#endif
    // move 后 GLFW user pointer 仍指向旧对象，回调会写悬垂对象；
    // 重新指向新对象（旧对象 handle_ 已置空，不会双重销毁）
    if (handle_) {
        glfwSetWindowUserPointer(handle_, this);
#ifdef _WIN32
        if (win32_hwnd_) {
            // 子类化仍在：把 USERDATA 改指向新对象，窗口关闭回调照常工作
            SetWindowLongPtrW(static_cast<HWND>(win32_hwnd_), GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(this));
        }
#endif
    }
}

Window& Window::operator=(Window&& o) noexcept {
    if (this != &o) {
        if (handle_) {
            glfwDestroyWindow(handle_);
        }
        handle_ = o.handle_;
        mode_ = o.mode_;
        title_ = std::move(o.title_);
        width_ = o.width_;
        height_ = o.height_;
        fps_ = o.fps_;
        delta_time_ = o.delta_time_;
        last_time_ = o.last_time_;
        frame_count_ = o.frame_count_;
        fps_interval_ = o.fps_interval_;
        o.handle_ = nullptr;
#ifdef _WIN32
        // 先还原旧窗口的 WndProc（若已子类化），避免销毁时按新对象解引用
        remove_resize_border();
        original_wndproc_ = o.original_wndproc_;
        win32_hwnd_ = o.win32_hwnd_;
        o.original_wndproc_ = nullptr;
        o.win32_hwnd_ = nullptr;
#endif
        // move 赋值后 GLFW user pointer 仍指向旧对象，重新指向新对象
        if (handle_) {
            glfwSetWindowUserPointer(handle_, this);
#ifdef _WIN32
            if (win32_hwnd_) {
                SetWindowLongPtrW(static_cast<HWND>(win32_hwnd_), GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(this));
            }
#endif
        }
    }
    return *this;
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------
bool Window::is_valid() const {
    return handle_ != nullptr;
}

bool Window::should_close() const {
    return handle_ ? glfwWindowShouldClose(handle_) : true;
}

void Window::request_close() {
    close_requested_ = true;
    if (handle_) {
        glfwSetWindowShouldClose(handle_, GLFW_TRUE);
    }
}

void Window::poll_events() {
    glfwPollEvents();
}

void Window::swap_buffers() {
    if (handle_) {
        glfwSwapBuffers(handle_);
    }
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------
void Window::set_title(const std::string& title) {
    title_ = title;
    if (handle_) {
        glfwSetWindowTitle(handle_, title.c_str());
    }
}

void Window::set_size(int width, int height) {
    width_ = width;
    height_ = height;
    if (handle_) {
        glfwSetWindowSize(handle_, width, height);
    }
}

void Window::set_decorated(bool decorated) {
    if (handle_) {
        glfwSetWindowAttrib(handle_, GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
    }
}

void Window::set_position(int x, int y) {
    if (handle_) {
        glfwSetWindowPos(handle_, x, y);
    }
}

void Window::center_on_primary_monitor() {
    if (!handle_) return;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) return;
        mx = 0; my = 0; mw = mode->width; mh = mode->height;
    }
    glfwSetWindowPos(handle_, mx + (mw - width_) / 2, my + (mh - height_) / 2);
}

void Window::focus_window() {
    if (handle_) {
        glfwShowWindow(handle_);
        glfwFocusWindow(handle_);
    }
}

void Window::get_size(int& width, int& height) const {
    if (handle_) {
        glfwGetWindowSize(handle_, &width, &height);
    } else {
        width = width_;
        height = height_;
    }
}

void Window::set_vsync(bool enabled) {
    if (!handle_) return;
    if (context_type_ != WindowContextType::OpenGL) return;

    // 只在当前线程已经持有本窗口 GL context 时才设置 swap interval，
    // 避免在 RenderContext::start() 之后从渲染线程抢回 context 导致崩溃。
    if (glfwGetCurrentContext() != handle_) {
        GLOG_WARN("Window::set_vsync: context not current on this thread, skipping");
        return;
    }

    glfwSwapInterval(enabled ? 1 : 0);
    GLOG_INFO("VSync {}", enabled ? "enabled" : "disabled");
}

void Window::set_cursor_visible(bool visible) {
    if (!handle_) return;
    CursorMode desired = visible ? CursorMode::Normal : CursorMode::Hidden;
    if (desired == cursor_mode_) return;
    cursor_mode_ = desired;
    glfwSetInputMode(handle_, GLFW_CURSOR,
        visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void Window::set_cursor_disabled(bool disabled) {
    if (!handle_) return;
    CursorMode desired = disabled ? CursorMode::Disabled : CursorMode::Normal;
    if (desired == cursor_mode_) return;
    cursor_mode_ = desired;
    glfwSetInputMode(handle_, GLFW_CURSOR,
        disabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::set_cursor(Cursor* cursor) {
    if (!handle_ || cursor == current_cursor_) return;
    current_cursor_ = cursor;
    glfwSetCursor(handle_, cursor ? cursor->native_handle() : nullptr);
}

void Window::set_cursor_pos(double x, double y) {
    if (handle_) {
        glfwSetCursorPos(handle_, x, y);
    }
}

void Window::minimize() {
    if (handle_) {
        glfwIconifyWindow(handle_);
    }
}

void Window::maximize() {
    if (handle_) {
        glfwMaximizeWindow(handle_);
    }
}

void Window::restore() {
    if (handle_) {
        glfwRestoreWindow(handle_);
    }
}

bool Window::is_maximized() const {
    if (!handle_) return false;
    return glfwGetWindowAttrib(handle_, GLFW_MAXIMIZED) == GLFW_TRUE;
}

// 无边框窗口拖动：把菜单栏上的左键按下转发给系统标题栏拖动逻辑。
// 只处理用户按下的第一帧（IsMouseClicked 仅按下的那一帧为 true），
// 之后系统接管鼠标，glfwPollEvents 仍会持续喂事件直到用户松开。
void Window::begin_caption_drag() {
    if (!handle_) return;
#ifdef _WIN32
    if (HWND hwnd = glfwGetWin32Window(handle_)) {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
}

// 无边框窗口边缘缩放：子类化 WndProc，在 WM_NCHITTEST 返回缩放 hit-test 码。
// 需要在窗口创建后（任何 GLFW 回调就绪后）调用；窗口销毁前调用 remove_resize_border() 还原。
void Window::install_resize_border() {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(handle_);
    if (!hwnd || original_wndproc_) return;
    win32_hwnd_ = hwnd;
    original_wndproc_ = reinterpret_cast<void*>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_resize_wndproc)));
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    GLOG_INFO("Window: resize border installed (frameless edge-resize)");
#endif
}

void Window::remove_resize_border() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(win32_hwnd_);
    if (hwnd && original_wndproc_) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wndproc_));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        original_wndproc_ = nullptr;
        GLOG_INFO("Window: resize border removed");
    }
#endif
}

void Window::set_resize_callback(ResizeCallback cb) {
    resize_callback_ = std::move(cb);
}

bool Window::has_focus() const {
    return handle_ ? glfwGetWindowAttrib(handle_, GLFW_FOCUSED) != 0 : false;
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
bool Window::get_key(int key) const {
    return handle_ ? (glfwGetKey(handle_, key) == GLFW_PRESS) : false;
}

void Window::get_cursor_pos(double& x, double& y) const {
    if (handle_) {
        glfwGetCursorPos(handle_, &x, &y);
    } else {
        x = 0.0;
        y = 0.0;
    }
}

GLFWwindow* Window::native_handle() const {
    return handle_;
}

// ---------------------------------------------------------------------------
// FPS / Delta Time
// ---------------------------------------------------------------------------
void Window::update_frame_stats() {
    double current_time = glfwGetTime();
    if (last_time_ == 0.0) {
        last_time_ = current_time;
    }
    delta_time_ = current_time - last_time_;
    last_time_ = current_time;

    frame_count_++;
    fps_interval_ += delta_time_;

    if (fps_interval_ >= 1.0) {
        fps_ = static_cast<double>(frame_count_) / fps_interval_;
        frame_count_ = 0;
        fps_interval_ = 0.0;
    }
}

double Window::fps() const {
    return fps_;
}

double Window::delta_time() const {
    return delta_time_;
}

} // namespace gryce_engine::platform