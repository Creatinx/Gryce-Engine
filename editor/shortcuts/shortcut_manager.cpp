#include "shortcut_manager.h"

namespace gryce_engine::editor {

void ShortcutManager::register_shortcut(const std::string& name, KeyCombo combo, Action action) {
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) {
        entries_[it->second] = Entry{combo, std::move(action), name};
    } else {
        name_to_index_[name] = entries_.size();
        entries_.push_back(Entry{combo, std::move(action), name});
    }
}

void ShortcutManager::unregister_shortcut(const std::string& name) {
    auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) return;

    size_t index = it->second;
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
    name_to_index_.erase(it);

    // 重建后续索引
    for (size_t i = index; i < entries_.size(); ++i) {
        name_to_index_[entries_[i].name] = i;
    }
}

void ShortcutManager::process() {
    ImGuiIO& io = ImGui::GetIO();
    // 文本输入框获焦时不触发全局快捷键
    if (io.WantCaptureKeyboard) return;

    for (const auto& entry : entries_) {
        if (combo_triggered(entry.combo)) {
            if (entry.action) entry.action();
            // 一个快捷键触发后不再继续检查，避免同一帧多个冲突动作
            break;
        }
    }
}

bool ShortcutManager::combo_triggered(const KeyCombo& combo) const {
    if (combo.key == ImGuiKey_None) return false;

    const ImGuiIO& io = ImGui::GetIO();
    if (combo.ctrl != io.KeyCtrl) return false;
    if (combo.shift != io.KeyShift) return false;
    if (combo.alt != io.KeyAlt) return false;

    // 防止按住不放时重复触发：要求本帧按下、上一帧未按下
    return ImGui::IsKeyPressed(combo.key, false);
}

} // namespace gryce_engine::editor
