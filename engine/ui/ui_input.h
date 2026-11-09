#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.h"
#include "ui/ui_geometry.h"

namespace gryce_engine::components {
class NodeUI;
}

namespace gryce_engine::ui {

// 与 GLFW 键值一致，UI 控件无需包含平台头文件。
enum class UIKey : int {
    A = 65,
    C = 67,
    V = 86,
    X = 88,
    Escape = 256,
    Enter = 257,
    Tab = 258,
    Backspace = 259,
    Delete = 261,
    Right = 262,
    Left = 263,
    Down = 264,
    Up = 265,
    Home = 268,
    End = 269,
};

struct UIKeyEvent {
    int key = 0;
    bool repeat = false;
};

// 一帧的输入快照。平台适配器负责把 GInput_* 的轮询状态转换成这个结构；
// UI 核心不直接依赖 GLFW 或平台 DLL。
struct UIInputFrame {
    uint64_t frame_serial = 0;
    float delta_time = 1.0f / 60.0f;
    math::Vector2f pointer;
    math::Vector2f wheel;
    bool pointer_down[3] = {false, false, false};
    bool pointer_pressed[3] = {false, false, false};
    bool pointer_released[3] = {false, false, false};

    int modifiers = 0;
    std::vector<UIKeyEvent> key_down;
    std::vector<int> key_up;
    std::vector<uint32_t> characters;
    std::string preedit;
};

enum class UIEventType : uint8_t {
    PointerEnter = 0,
    PointerLeave,
    PointerMove,
    PointerDown,
    PointerUp,
    Click,
    FocusGained,
    FocusLost,
    KeyDown,
    KeyUp,
    Character,
    Wheel,
    Preedit,
    DragStart,
    DragOver,
    Drop,
    DragEnd,
};

struct UIEvent {
    UIEventType type = UIEventType::PointerMove;
    components::NodeUI* target = nullptr;
    components::NodeUI* current_target = nullptr;
    math::Vector2f position;
    int mouse_button = -1;
    int key = 0;
    uint32_t codepoint = 0;
    math::Vector2f wheel;
    std::string preedit;
    std::string payload;
    UIRect layout_rect;
    UIRect parent_rect;
    int modifiers = 0;
    bool repeat = false;
    bool handled = false;
};

} // namespace gryce_engine::ui
