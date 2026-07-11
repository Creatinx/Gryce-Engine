#pragma once

#include <functional>
#include <string>

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
class Window {
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
    void set_vsync(bool enabled);
    void set_cursor_visible(bool visible);
    void set_cursor_disabled(bool disabled);
    void set_cursor(Cursor* cursor);

    void set_cursor_pos(double x, double y);

    // 窗口大小变化回调
    using ResizeCallback = std::function<void(int width, int height)>;
    void set_resize_callback(ResizeCallback cb);

    // 焦点
    bool has_focus() const;

    // 输入
    bool get_key(int key) const;
    void get_cursor_pos(double& x, double& y) const;

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
    enum class CursorMode { Normal, Hidden, Disabled };
    CursorMode cursor_mode_ = CursorMode::Normal;

    GLFWwindow* handle_ = nullptr;
    Cursor* current_cursor_ = nullptr;
    WindowMode mode_;
    WindowContextType context_type_ = WindowContextType::OpenGL;
    std::string title_;
    int width_ = 0;
    int height_ = 0;
    ResizeCallback resize_callback_;
    bool close_requested_ = false;

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
