#include "window.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#include <GLFW/glfw3.h>

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
    #include <emscripten/html5.h>
    #include <algorithm>
#endif

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
    // 单独按 Alt 会激活窗口的系统菜单（进入菜单模式），之后键盘事件被系统菜单
    // 接管：表现为"按一下 Alt 之后 Alt 就失灵了、松开也收不到 UP"，随后所有
    // Alt 都失效（GLFW 的按键状态卡在按下）。游戏/工具窗口不需要菜单栏，这里
    // 直接吞掉 SC_KEYMENU，让 Alt 始终作为普通按键送到窗口。
    if (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_KEYMENU) {
        return 0;
    }
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

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// 浏览器视口：画布铺满页面
//
// Web 端没有"窗口"，页面就是窗口：壳用 CSS 把画布撑满视口
// （width:100vw; height:100vh）。但 WebGL 的渲染缓冲（canvas.width/height）
// 不会跟着 CSS 走 —— 浏览器只会把现有缓冲拉伸到 CSS 尺寸，结果是画面比例
// 失真、且按 CSS 尺寸低清显示。所以窗口建好后要把缓冲同步成页面尺寸，并随
// 浏览器窗口 resize 继续跟随。
//
// 指针坐标也依赖这一点：Emscripten 的 GLFW 用 canvas.width / CSS 宽度 折算
// 鼠标位置，缓冲与 CSS 尺寸一致时该系数恰好是渲染倍率，指针落点才准。
// ---------------------------------------------------------------------------
namespace {

constexpr double k_web_max_render_scale = 2.0;

// 当前窗口句柄与 resize 合并定时器（见下方 web_on_page_resize）。
GLFWwindow* web_window_handle = nullptr;
long web_resize_timer = 0;

// 渲染倍率：1 = CSS 像素（默认）。高分屏想换原生像素清晰度时，壳可以从
// URL 的 ?rs= 填到 Module.gryceRenderScale（开销按像素面积增长）。
EM_JS(double, gryce_web_render_scale, (), {
    if (typeof Module === 'undefined' || typeof Module['gryceRenderScale'] !== 'number') return 1.0;
    var s = Module['gryceRenderScale'];
    return (s > 0.1 && s < 4.0) ? s : 1.0;
});

// ---------------------------------------------------------------------------
// 鼠标位移（pointer lock）
//
// 两个只在浏览器里成立的坑叠在一起，合起来就是"网页里鼠标完全没反应"：
//  1) Emscripten 的 GLFW 从不设置 GLFW_FOCUSED（libglfw.js 里没有接 focus/blur
//     事件，属性默认 0），glfwGetWindowAttrib 恒返回"失焦" → 引擎的指针锁定
//     策略（有焦点才锁）永远不触发。这个在 Window::has_focus() 里改问
//     document.hasFocus()。
//  2) 即便锁上了，pointer lock 期间浏览器把光标坐标钉死（clientX/Y 不再变化），
//     真实位移只在 movementX/Y 里；而 libglfw.js 的 calculateMouseCoords 只拿
//     clientX/Y 减画布偏移，glfwGetCursorPos 于是恒返回同一个值 → 视角转不动。
//
// 所以这里在 JS 侧自己攒 movementX/Y，由 Window::take_pointer_delta() 每帧取走。
// ---------------------------------------------------------------------------
EM_JS(void, gryce_install_pointer_delta, (), {
    if (typeof Module === 'undefined' || Module['grycePointerDeltaInstalled']) return;
    Module['grycePointerDeltaInstalled'] = true;
    Module['grycePointerDx'] = 0;
    Module['grycePointerDy'] = 0;
    var canvas = Module['canvas'] || document.getElementById('canvas');
    if (!canvas) return;
    // 只收画布上的事件：调试模式下页面下半是日志面板，在日志上拖滚动条不该转视角。
    // pointer lock 生效时鼠标事件的目标就是被锁定的画布，一条路径覆盖两种情况；
    // 未锁定时浏览器照样按"上一次事件到这一次"算 movementX/Y，拖动同样有效。
    canvas.addEventListener('mousemove', function (e) {
        Module['grycePointerDx'] += (e.movementX || 0);
        Module['grycePointerDy'] += (e.movementY || 0);
    }, { passive: true });
});

EM_JS(double, gryce_take_pointer_dx, (), {
    if (typeof Module === 'undefined') return 0.0;
    var v = Module['grycePointerDx'] || 0.0;
    Module['grycePointerDx'] = 0.0;
    return v;
});

EM_JS(double, gryce_take_pointer_dy, (), {
    if (typeof Module === 'undefined') return 0.0;
    var v = Module['grycePointerDy'] || 0.0;
    Module['grycePointerDy'] = 0.0;
    return v;
});

// 页面级焦点：GLFW 属性拿不到（见上），只能问 document。
EM_JS(int, gryce_web_document_focused, (), {
    return (typeof document !== 'undefined' && document.hasFocus && document.hasFocus()) ? 1 : 0;
});

// 画布选择器：#canvas 与 Emscripten 默认壳一致；取不到 CSS 尺寸时退回标签名。
const char* web_canvas_selector() {
    static const char* cached = nullptr;
    if (cached) return cached;
    double w = 0.0;
    double h = 0.0;
    cached = (emscripten_get_element_css_size("#canvas", &w, &h) == EMSCRIPTEN_RESULT_SUCCESS)
                 ? "#canvas"
                 : "canvas";
    return cached;
}

void web_fit_canvas_to_page() {
    if (web_window_handle == nullptr) return;
    const char* sel = web_canvas_selector();
    double css_w = 0.0;
    double css_h = 0.0;
    const EMSCRIPTEN_RESULT r = emscripten_get_element_css_size(sel, &css_w, &css_h);
    if (r != EMSCRIPTEN_RESULT_SUCCESS) {
        GLOG_WARN("Window: canvas CSS size query failed ({}), keeping requested size",
                  static_cast<int>(r));
        return;
    }
    if (css_w < 1.0 || css_h < 1.0) {
        GLOG_WARN("Window: canvas CSS size is {}x{}, page not laid out yet", css_w, css_h);
        return;
    }

    const double scale = std::min(gryce_web_render_scale(), k_web_max_render_scale);
    const int w = std::max(1, static_cast<int>(css_w * scale + 0.5));
    const int h = std::max(1, static_cast<int>(css_h * scale + 0.5));
    GLOG_INFO("Window: canvas css {}x{} → buffer {}x{} (scale {})", css_w, css_h, w, h, scale);
    // 必须走 glfwSetWindowSize 而不是 emscripten_set_canvas_element_size：
    // 后者只改 canvas.width/height，GLFW 侧记录的窗口尺寸（指针坐标换算的
    // 基准）和 framebuffer 尺寸都不会更新，也不会触发 resize 回调。
    glfwSetWindowSize(web_window_handle, w, h);
}

// 一次拖拽会连发几十个 resize 事件，而重建渲染目标是 GPU 资源操作，
// 逐次执行会明显卡顿：合并成 80ms 后的单次同步。
EM_BOOL web_on_page_resize(int, const EmscriptenUiEvent*, void*) {
    if (web_resize_timer == 0) {
        web_resize_timer = emscripten_set_timeout([](void*) {
            web_resize_timer = 0;
            web_fit_canvas_to_page();
        }, 80, nullptr);
    }
    return EM_TRUE;
}

void web_track_page_size(GLFWwindow* handle) {
    web_window_handle = handle;
    static bool installed = false;
    if (installed) return;
    installed = true;
    gryce_install_pointer_delta();
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false,
                                   web_on_page_resize);
}

}  // namespace
#endif // __EMSCRIPTEN__

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

#ifdef __EMSCRIPTEN__
    // Emscripten 的 GLFW 端口：显式请求 WebGL2（= GLES 3.0）context。
    // 不设版本 hint 时端口可能给 WebGL1（没有 instancing / depth texture /
    // UBO，等于不可用）。配合链接选项 -sMAX_WEBGL_VERSION=2 -sFULL_ES3。
    // 桌面构建不进这段（GLFW_OPENGL_ES_API 在桌面会直接创建失败）。
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

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
    // 字符输入：唯一的真实字符通道（大小写、输入法、粘贴都靠它）。
    // 注意：ImGui 的 GLFW 后端初始化时会装自己的 char 回调，覆盖这一条——
    // 那时内置 UI 轨拿不到字符（两条轨本来就不该同时跑）。
    glfwSetCharCallback(handle_, [](GLFWwindow* w, unsigned int codepoint) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self) self->push_char(codepoint);
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

#ifdef __EMSCRIPTEN__
    // 浏览器里"窗口"就是页面：缓冲先对齐到画布当前的显示尺寸，之后跟随 resize。
    web_track_page_size(handle_);
    web_fit_canvas_to_page();
    {
        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(handle_, &fb_w, &fb_h);
        width_ = fb_w;
        height_ = fb_h;
        GLOG_INFO("Window: canvas fitted to page ({}x{})", fb_w, fb_h);
    }
#endif

    // 无边框窗口边缘缩放边框（Win32 WndProc 子类化）。
    // GLFW 无边框窗口用 WS_POPUP（无 WS_THICKFRAME），不装这个就没有拖边缩放。
#ifdef _WIN32
    install_resize_border();
#endif
}

Window::~Window() {
    destroy();
}

void Window::destroy() {
    if (handle_) {
#ifdef _WIN32
        remove_resize_border();
#endif
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

void Window::set_visible(bool visible) {
    if (!handle_) return;
    if (visible) {
        glfwShowWindow(handle_);
        glfwFocusWindow(handle_);
    } else {
        glfwHideWindow(handle_);
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

void Window::get_framebuffer_size(int& width, int& height) const {
    if (handle_) {
        glfwGetFramebufferSize(handle_, &width, &height);
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
    // 不做"和上次相同就跳过"的缓存判断：GLFW/系统会在窗口失焦等情况下自行
    // 改动光标模式，本地缓存一旦与实际失同步，跳过就会变成静默失效（表现为
    // "有时候锁得住、大部分时候锁不住"）。glfwSetInputMode 本身幂等且开销极小，
    // 每次都实际下发最稳妥。
    glfwSetInputMode(handle_, GLFW_CURSOR,
        visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void Window::set_cursor_disabled(bool disabled) {
    if (!handle_) return;
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
#ifdef _WIN32
void Window::install_resize_border() {
    HWND hwnd = glfwGetWin32Window(handle_);
    if (!hwnd || original_wndproc_) return;
    win32_hwnd_ = hwnd;
    original_wndproc_ = reinterpret_cast<void*>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_resize_wndproc)));
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    GLOG_INFO("Window: resize border installed (frameless edge-resize)");
}

void Window::remove_resize_border() {
    HWND hwnd = static_cast<HWND>(win32_hwnd_);
    if (hwnd && original_wndproc_) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wndproc_));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        original_wndproc_ = nullptr;
        GLOG_INFO("Window: resize border removed");
    }
}
#endif

void Window::set_resize_callback(ResizeCallback cb) {
    resize_callback_ = std::move(cb);
}

bool Window::has_focus() const {
#ifdef __EMSCRIPTEN__
    // Emscripten 的 GLFW 从不设置 GLFW_FOCUSED（没接 focus/blur 事件，属性恒为 0），
    // 照搬桌面实现会让浏览器里永远判定"失焦"：指针锁定不下发、鼠标彻底失效。
    return handle_ ? gryce_web_document_focused() != 0 : false;
#else
    return handle_ ? glfwGetWindowAttrib(handle_, GLFW_FOCUSED) != 0 : false;
#endif
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
bool Window::get_key(int key) const {
    return handle_ ? (glfwGetKey(handle_, key) == GLFW_PRESS) : false;
}

void Window::push_char(unsigned int codepoint) {
    if (char_count_ >= kCharQueueCap) return;   // 满了丢新字符（见 window.h）
    char_queue_[(char_head_ + char_count_) % kCharQueueCap] = codepoint;
    ++char_count_;
}

int Window::pull_chars(unsigned int* out, int cap) {
    if (!out || cap <= 0) return 0;
    const int n = char_count_ < cap ? char_count_ : cap;
    for (int i = 0; i < n; ++i) {
        out[i] = char_queue_[(char_head_ + i) % kCharQueueCap];
    }
    char_head_  = (char_head_ + n) % kCharQueueCap;
    char_count_ -= n;
    // 没被取走的（cap 不够）留在队列里，下一帧继续
    return n;
}

void Window::get_cursor_pos(double& x, double& y) const {
    if (handle_) {
        glfwGetCursorPos(handle_, &x, &y);
    } else {
        x = 0.0;
        y = 0.0;
    }
}

bool Window::take_pointer_delta(double& dx, double& dy) {
#ifdef __EMSCRIPTEN__
    if (handle_ == nullptr) return false;
    const double x = gryce_take_pointer_dx();
    const double y = gryce_take_pointer_dy();
    if (x == 0.0 && y == 0.0) return false;
    dx = x;
    dy = y;
    return true;
#else
    // 桌面端指针位移直接由 get_cursor_pos 的绝对值差分得到（配合回中），
    // 这里保持 API 表面一致但不参与计算。
    (void)dx;
    (void)dy;
    return false;
#endif
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
