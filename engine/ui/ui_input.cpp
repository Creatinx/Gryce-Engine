// ===========================================================================
// engine/ui/ui_input.cpp — 输入捕获门控与快捷键表的实现
//
// want_capture_* 是唯一需要 ImGui 的地方（读 IO 状态）；无 ImGui 的构建里
// 恒 false——没有 UI 在跑，也就没有 UI 在吃键盘。
// ===========================================================================

#include "ui/ui_input.h"

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace gryce_ui {

bool wants_capture_keyboard() {
#ifdef GRYCE_HAS_IMGUI
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
#else
    return false;
#endif
}

bool wants_capture_mouse() {
#ifdef GRYCE_HAS_IMGUI
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::GetIO().WantCaptureMouse;
#else
    return false;
#endif
}

int ShortcutTable::add(uint32_t key, uint32_t mods, int command) {
    if (has_conflict(key, mods)) return 0;
    if (count_ >= kCapacity) return 0;
    entries_[count_].key = key;
    entries_[count_].mods = mods;
    entries_[count_].command = command;
    ++count_;
    return 1;
}

int ShortcutTable::find(uint32_t key, uint32_t mods) const {
    for (int i = 0; i < count_; ++i) {
        if (entries_[i].key == key && entries_[i].mods == mods) {
            return entries_[i].command;
        }
    }
    return -1;
}

bool ShortcutTable::has_conflict(uint32_t key, uint32_t mods) const {
    return find(key, mods) >= 0;
}

void ShortcutTable::clear() { count_ = 0; }

int ShortcutTable::size() const { return count_; }

} // namespace gryce_ui
