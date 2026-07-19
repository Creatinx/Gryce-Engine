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
}

Window::~Window() {
    destroy();
}

void Window::destroy() {
    if (handle_) {
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
    // move 后 GLFW user pointer 仍指向旧对象，回调会写悬垂对象；
    // 重新指向新对象（旧对象 handle_ 已置空，不会双重销毁）
    if (handle_) {
        glfwSetWindowUserPointer(handle_, this);
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
        // move 赋值后 GLFW user pointer 仍指向旧对象，重新指向新对象
        if (handle_) {
            glfwSetWindowUserPointer(handle_, this);
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