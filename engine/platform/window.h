#pragma once

#include <functional>
#include <string>

#include "export.h"

struct GLFWwindow; // 前向声明，避免暴露 GLFW 头文件

namespace gryce_engine::platform {

class Cursor;

// ---------------------------------------------------------------------------
// 窗口模式
// ---------------------------------------------------------------------------
enum class WindowMode {
    Windowed,
    Borderless,
    Fullscreen,
    Maximized
};

// 窗口是否创建 OpenGL context；Vulkan/NoAPI 模式传 NoApi
enum class WindowContextType {
    OpenGL,
    NoApi
};

// ---------------------------------------------------------------------------
// Window — GLFW 窗口封装
// ---------------------------------------------------------------------------
class GRYCE_API Window {
public:
    Window(const std::string& title, int width, int height, WindowMode mode,
           WindowContextType context_type = WindowContextType::OpenGL);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;

    bool is_valid() const;

    // 显式销毁窗口（比析构更可控，建议在 glfwTerminate 之前调用）
    void destroy();

    // 事件循环
    bool should_close() const;
    bool close_requested() const { return close_requested_; }
    void request_close();
    void poll_events();
    void swap_buffers();

    // 属性
    void set_title(const std::string& title);
    void set_size(int width, int height);
    void get_size(int& width, int& height) const;
    void get_framebuffer_size(int& width, int& height) const;
    // 切换 OS 标题栏/边框（GLFW_DECORATED），启动画面用无边框，编辑器恢复有边框
    void set_decorated(bool decorated);
    // 设置窗口位置（屏幕坐标）
    void set_position(int x, int y);
    // 将窗口居中到主显示器工作区
    void center_on_primary_monitor();
    // 置前并请求焦点
    void focus_window();
    // 显隐窗口（启动画面阶段隐藏主窗口，初始化完成后显示）
    void set_visible(bool visible);
    void set_vsync(bool enabled);
    void set_cursor_visible(bool visible);
    void set_cursor_disabled(bool disabled);
    void set_cursor(Cursor* cursor);

    void set_cursor_pos(double x, double y);

    // 无边框窗口控制（自定义标题栏用）
    void minimize();
    void maximize();
    void restore();
    bool is_maximized() const;
    // 无边框窗口拖动：将鼠标按下事件转换为系统标题栏拖动
    void begin_caption_drag();
    // 无边框窗口边缘缩放子类化：原始 WndProc（win32 专用，供 window.cpp 内 WndProc 使用）
#ifdef _WIN32
    void* native_wndproc() const { return original_wndproc_; }
#else
    // 非 Win32 平台没有 WndProc 子类化概念，恒返回 nullptr（保持 API 表面一致）
    void* native_wndproc() const { return nullptr; }
#endif

    // 窗口大小变化回调
    using ResizeCallback = std::function<void(int width, int height)>;
    void set_resize_callback(ResizeCallback cb);

    // 焦点
    bool has_focus() const;

    // 输入
    bool get_key(int key) const;
    void get_cursor_pos(double& x, double& y) const;

    // 文本输入（Unicode 码点）：GLFW 的字符回调是事件式的（一帧可能多次），
    // 这里积累成小环形队列，由 InputManager 每帧取走。存在的理由是内置 UI 的
    // 输入框需要真正的字符通道——按 glfwGetKey 反推字符拿不到大小写/输入法。
    static constexpr int kCharQueueCap = 64;
    void push_char(unsigned int codepoint);
    // 取走全部待处理码点，返回实际条数（队列随之清空）。
    int  pull_chars(unsigned int* out, int cap);
    // 指针位移增量（仅 Web 有真实数据）。浏览器 pointer lock 期间光标坐标被钉死，
    // GLFW 只搬 clientX/Y，位移只能从 JS 侧的 movementX/Y 累积里取；取走即清零。
    // 返回 false 表示这一帧没有位移（或非 Web 平台，调用方走 get_cursor_pos 差分）。
    bool take_pointer_delta(double& dx, double& dy);

    // 原生句柄
    GLFWwindow* native_handle() const;

    // FPS / 时间
    void update_frame_stats();
    double fps() const;
    double delta_time() const;

    // GLFW SDK 管理
    static bool init_sdk();
    static void shutdown_sdk();

private:
    GLFWwindow* handle_ = nullptr;
    Cursor* current_cursor_ = nullptr;
    WindowMode mode_;
    WindowContextType context_type_ = WindowContextType::OpenGL;
    std::string title_;
    int width_ = 0;
    int height_ = 0;
    ResizeCallback resize_callback_;
    bool close_requested_ = false;

    // 字符队列（单生产者 GLFW 回调 / 单消费者主线程）。满了丢新字符：
    // 一帧 64 个字符已经是粘贴级别的量，溢出说明消费端没在跑。
    unsigned int char_queue_[kCharQueueCap] = {};
    int char_head_ = 0;
    int char_count_ = 0;

#ifdef _WIN32
    // 无边框窗口边缘缩放的 WndProc 子类化（GLFW 无边框用 WS_POPUP，无 WS_THICKFRAME
    // 无系统缩放边框，必须由 WM_NCHITTEST 自行返回 HTLEFT/HTRIGHT/... 触发缩放）
    void install_resize_border();
    void remove_resize_border();
    void* original_wndproc_ = nullptr;   // 原始 WndProc
    void* win32_hwnd_ = nullptr;          // HWND 缓存，避免重复查询
#endif

    double fps_ = 0.0;
    double delta_time_ = 0.0;
    double last_time_ = 0.0;
    int frame_count_ = 0;
    double fps_interval_ = 0.0;

    // 外部函数：创建 GLFW 原生窗口
    GLFWwindow* create_glfw_window(const std::string& title, int w, int h, WindowMode mode,
                                   WindowContextType context_type);
};

} // namespace gryce_engine::platform
