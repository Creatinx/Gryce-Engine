// ===========================================================================
// editor/ext/input/shortcuts.h — 快捷键表
//
// 键位表放在 ext 而不是 main.js：同一份表既能被 UI 读去做键位提示，也能做
// 冲突检测；写在 JS 里就只能靠人肉保证不重复，两处各写一份必然漂移。
//
// 边沿检测用 GInput_IsKeyPressed（引擎侧的键状态与上一帧比较），
// 不用 engine.input.key_down 的"按住"语义 —— 后者会让一个键连发。
// ===========================================================================

#ifndef GRYCE_EDITOR_SHORTCUTS_H
#define GRYCE_EDITOR_SHORTCUTS_H

#include <string>

namespace gryce::editor::shortcuts {

// mods 位掩码：1 = Ctrl，2 = Shift，4 = Alt。
enum : int {
    kCtrl = 1,
    kShift = 2,
    kAlt = 4,
};

void reset();

// 绑定。同名的重复绑定视为改键，返回 1；
// 同一个 键+修饰 被别的动作占用时拒绝，返回 0（冲突必须显式解决）。
int bind(const char* name, int key, int mods);

// 本帧触发：["undo","translate"]。没有返回 "[]"。
std::string poll_json();

// 当前绑定表：[{"name":"undo","key":90,"mods":1}]，UI 拿去做提示。
std::string table_json();

} // namespace gryce::editor::shortcuts

#endif // GRYCE_EDITOR_SHORTCUTS_H
