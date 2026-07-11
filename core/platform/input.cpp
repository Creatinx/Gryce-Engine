#include "input.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <imm.h>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#include "platform/window.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::platform {

// Windows 下 IME 管理：鼠标锁定时彻底取消 IME 关联，解锁时恢复
#ifdef _WIN32
static HIMC g_saved_himc = nullptr;

static void disable_ime(HWND hwnd) {
    if (!hwnd) return;
    // 先关闭 IME 打开状态
    HIMC himc = ImmGetContext(hwnd);
    if (himc) {
        ImmSetOpenStatus(himc, FALSE);
        ImmReleaseContext(hwnd, himc);
    }
    // 彻底取消窗口与 IME 的关联（最强力，阻止任何输入法快捷键）
    g_saved_himc = ImmAssociateContext(hwnd, nullptr);
}

static void enable_ime(HWND hwnd) {
    if (!hwnd) return;
    if (g_saved_himc) {
        ImmAssociateContext(hwnd, g_saved_himc);
        g_saved_himc = nullptr;
    }
    HIMC himc = ImmGetContext(hwnd);
    if (himc) {
        ImmSetOpenStatus(himc, TRUE);
        ImmReleaseContext(hwnd, himc);
    }
}
#endif

InputManager::InputManager() {
    std::memset(keys_current_, 0, sizeof(keys_current_));
    std::memset(keys_previous_, 0, sizeof(keys_previous_));
    std::memset(mouse_buttons_current_, 0, sizeof(mouse_buttons_current_));
    std::memset(mouse_buttons_previous_, 0, sizeof(mouse_buttons_previous_));
}

void InputManager::update(Window* window) {
    window_ = window;
    if (!window || !window->is_valid()) return;

    // 保存上一帧状态
    std::memcpy(keys_previous_, keys_current_, sizeof(keys_current_));
    std::memcpy(mouse_buttons_previous_, mouse_buttons_current_, sizeof(mouse_buttons_current_));

    // 更新按键状态
    for (int i = 0; i < KEY_COUNT; ++i) {
        keys_current_[i] = window->get_key(i);
    }

    // 更新鼠标按键
    for (int i = 0; i < 8; ++i) {
        mouse_buttons_current_[i] = (glfwGetMouseButton(
            static_cast<GLFWwindow*>(window->native_handle()), i) == GLFW_PRESS);
    }

    // 更新鼠标位置（仅在窗口有焦点时）
    if (window->has_focus()) {
        double x, y;
        window->get_cursor_pos(x, y);
        mouse_x_ = x;
        mouse_y_ = y;

        if (first_mouse_) {
            mouse_prev_x_ = x;
            mouse_prev_y_ = y;
            first_mouse_ = false;
        }

        mouse_delta_x_ = x - mouse_prev_x_;
        mouse_delta_y_ = y - mouse_prev_y_;

        // 如果鼠标锁定，重置到窗口中心
        if (mouse_locked_) {
            int w, h;
            window->get_size(w, h);
            double center_x = w / 2.0;
            double center_y = h / 2.0;
            glfwSetCursorPos(static_cast<GLFWwindow*>(window->native_handle()), center_x, center_y);
            mouse_prev_x_ = center_x;
            mouse_prev_y_ = center_y;
        } else {
            mouse_prev_x_ = x;
            mouse_prev_y_ = y;
        }
    } else {
        // 无焦点：delta 归零，不更新鼠标位置
        mouse_delta_x_ = 0.0;
        mouse_delta_y_ = 0.0;
        first_mouse_ = true;
    }

#ifdef _WIN32
    // Windows: 防御性检查，鼠标锁定时确保 IME 始终关闭
    if (mouse_locked_ && window && window->is_valid() && window->has_focus()) {
        HWND hwnd = glfwGetWin32Window(static_cast<GLFWwindow*>(window->native_handle()));
        if (hwnd) {
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                if (ImmGetOpenStatus(himc)) {
                    ImmSetOpenStatus(himc, FALSE);
                }
                ImmReleaseContext(hwnd, himc);
            }
            // 确保 IME 关联始终为空
            if (!g_saved_himc) {
                g_saved_himc = ImmAssociateContext(hwnd, nullptr);
            }
        }
    }
#endif
}

bool InputManager::is_key_pressed(int key) const {
    if (key < 0 || key >= KEY_COUNT) return false;
    return keys_current_[key] && !keys_previous_[key];
}

bool InputManager::is_key_held(int key) const {
    if (key < 0 || key >= KEY_COUNT) return false;
    return keys_current_[key];
}

bool InputManager::is_key_released(int key) const {
    if (key < 0 || key >= KEY_COUNT) return false;
    return !keys_current_[key] && keys_previous_[key];
}

bool InputManager::is_mouse_button_pressed(int button) const {
    if (button < 0 || button >= 8) return false;
    return mouse_buttons_current_[button] && !mouse_buttons_previous_[button];
}

bool InputManager::is_mouse_button_held(int button) const {
    if (button < 0 || button >= 8) return false;
    return mouse_buttons_current_[button];
}

bool InputManager::is_mouse_button_released(int button) const {
    if (button < 0 || button >= 8) return false;
    return !mouse_buttons_current_[button] && mouse_buttons_previous_[button];
}

void InputManager::set_mouse_locked(bool locked) {
    if (mouse_locked_ == locked) return;
    mouse_locked_ = locked;
    first_mouse_ = true;

    if (window_ && window_->is_valid()) {
        GLFWwindow* handle = static_cast<GLFWwindow*>(window_->native_handle());
        if (locked) {
            window_->set_cursor_disabled(true);
            // 立即重置到中心
            int w, h;
            window_->get_size(w, h);
            glfwSetCursorPos(handle, w / 2.0, h / 2.0);
            // Windows: 彻底取消 IME 关联，阻止 Shift 切换输入法
#ifdef _WIN32
            disable_ime(glfwGetWin32Window(handle));
            GLOG_INFO("IME disabled (mouse locked)");
#endif
        } else {
            window_->set_cursor_disabled(false);
            // Windows: 恢复 IME 关联
#ifdef _WIN32
            enable_ime(glfwGetWin32Window(handle));
            GLOG_INFO("IME restored (mouse unlocked)");
#endif
        }
    }
}

} // namespace gryce_engine::platform
