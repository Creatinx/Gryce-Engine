#pragma once

#include <cstdint>
#include <cstring>

#include "export.h"

struct GLFWwindow;

namespace gryce_engine::platform {

class Window;

// ---------------------------------------------------------------------------
// InputManager — IMB (Input Management Buffer) 修复
// 跟踪按键状态变化、鼠标 delta、支持鼠标锁定
// ---------------------------------------------------------------------------
class GRYCE_API InputManager {
public:
    InputManager();
    ~InputManager() = default;

    // 每帧在主线程开头调用
    void update(Window* window);

    // 按键状态
    bool is_key_pressed(int key) const;  // 本帧刚按下
    bool is_key_held(int key) const;     // 按住
    bool is_key_released(int key) const; // 本帧刚释放

    // 鼠标
    double mouse_x() const { return mouse_x_; }
    double mouse_y() const { return mouse_y_; }
    double mouse_delta_x() const { return mouse_delta_x_; }
    double mouse_delta_y() const { return mouse_delta_y_; }

    bool is_mouse_button_pressed(int button) const;
    bool is_mouse_button_held(int button) const;
    bool is_mouse_button_released(int button) const;

    // 鼠标锁定（FPS 模式）
    void set_mouse_locked(bool locked);
    bool is_mouse_locked() const { return mouse_locked_; }

    void reset_mouse_deltas() { mouse_delta_x_ = 0.0; mouse_delta_y_ = 0.0; }

private:
    Window* window_ = nullptr;

    static constexpr int KEY_COUNT = 512;
    bool keys_current_[KEY_COUNT] = {false};
    bool keys_previous_[KEY_COUNT] = {false};

    bool mouse_buttons_current_[8] = {false};
    bool mouse_buttons_previous_[8] = {false};

    double mouse_x_ = 0.0;
    double mouse_y_ = 0.0;
    double mouse_prev_x_ = 0.0;
    double mouse_prev_y_ = 0.0;
    double mouse_delta_x_ = 0.0;
    double mouse_delta_y_ = 0.0;

    bool mouse_locked_ = false;
    bool first_mouse_ = true;

    void update_key_state(int key, bool current);
    void update_mouse_button_state(int button, bool current);
};

} // namespace gryce_engine::platform
