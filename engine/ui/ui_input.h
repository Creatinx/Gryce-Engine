// ===========================================================================
// engine/ui/ui_input.h — 输入捕获门控与快捷键表（无 ImGui 依赖的纯逻辑）
//
// 解决的问题（编辑器真实踩过）：输入框里打 "Wire"，快捷键把工具切走了——
// 宿主不知道键盘被 UI 吃着。规则必须是引擎策略而不是每个宿主各自记得：
// 派发任何快捷键之前先问 UI 要不要这个键盘。
//
// 第二部分是快捷键表：注册 (键, 修饰键) → 命令，重复注册算冲突。
// 冲突必须在注册期就报出来，不能等用户按下去才发现两个功能抢同一个键。
//
// 线程模型：宿主线程（帧外）。want_capture_* 在 ImGui 构建里读 ImGui 的
// IO，无 ImGui 构建恒为 false（那时没有 UI 在吃键盘）。
// ===========================================================================

#ifndef GRYCE_UI_INPUT_H
#define GRYCE_UI_INPUT_H

#include <cstdint>

namespace gryce_ui {

// UI 是否正吃着键盘 / 鼠标（ImGui 的 WantCaptureKeyboard / WantCaptureMouse）。
// 宿主据此决定"这次按键还给不给场景导航 / 快捷键"。
bool wants_capture_keyboard();
bool wants_capture_mouse();

// 引擎策略：UI 吃着键盘时不派发任何快捷键。一行实现，但必须是**所有**
// 宿主的唯一入口——各自判断就会各自漏。
inline bool should_dispatch_shortcut() { return !wants_capture_keyboard(); }

// 修饰键位（与 ui_api.h 的 GUI_MOD_* 同值，避免两个枚举漂移）。
enum UiMod : uint32_t {
    UI_MOD_CTRL  = 1u << 0,
    UI_MOD_SHIFT = 1u << 1,
    UI_MOD_ALT   = 1u << 2,
    UI_MOD_SUPER = 1u << 3,
};

class ShortcutTable {
public:
    static constexpr int kCapacity = 64;

    struct Entry {
        uint32_t key     = 0;   // 宿主键码（GLFW_KEY_* 这类，本层不认识语义）
        uint32_t mods    = 0;   // UI_MOD_* 位或
        int      command = 0;   // 调用方自定义命令号
    };

    // 注册。返回 1 = 成功；0 = 同一个 (key,mods) 已存在（冲突）或表满。
    // 冲突不覆盖、不报警静默：调用方要能看见失败。
    int add(uint32_t key, uint32_t mods, int command);
    // 查命令号，没有返回 -1。精确匹配（修饰键必须完全一致）。
    int find(uint32_t key, uint32_t mods) const;
    // (key,mods) 是否已注册（注册前自查用）。
    bool has_conflict(uint32_t key, uint32_t mods) const;

    void clear();
    int  size() const;

    const Entry& at(int i) const { return entries_[i]; }

private:
    Entry entries_[kCapacity];
    int   count_ = 0;
};

} // namespace gryce_ui

#endif // GRYCE_UI_INPUT_H
