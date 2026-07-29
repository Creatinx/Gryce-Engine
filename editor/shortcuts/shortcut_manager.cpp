#include "shortcut_manager.h"

#include <cctype>
#include <sstream>

namespace gryce_engine::editor {

void ShortcutManager::register_shortcut(const std::string& name, KeyCombo combo, Action action) {
    // 首次注册的组合键记为默认值，供"恢复默认"使用
    defaults_.try_emplace(name, combo);
    auto it = name_to_index_.find(name);
    if (it != name_to_index_.end()) {
        // 覆盖注册时保留当前（可能被用户重绑定过的）组合键
        KeyCombo current = entries_[it->second].combo;
        entries_[it->second] = Entry{current, std::move(action), name};
    } else {
        name_to_index_[name] = entries_.size();
        entries_.push_back(Entry{combo, std::move(action), name});
    }
}

bool ShortcutManager::set_combo(const std::string& name, const KeyCombo& combo) {
    auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) return false;
    entries_[it->second].combo = combo;
    return true;
}

bool ShortcutManager::combo_by_name(const std::string& name, KeyCombo& out) const {
    auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) return false;
    out = entries_[it->second].combo;
    return true;
}

bool ShortcutManager::reset_to_default(const std::string& name) {
    auto d = defaults_.find(name);
    if (d == defaults_.end()) return false;
    return set_combo(name, d->second);
}

void ShortcutManager::reset_all_defaults() {
    for (const auto& [name, combo] : defaults_) {
        set_combo(name, combo);
    }
}

std::string ShortcutManager::conflict_of(const KeyCombo& combo, const std::string& exclude_name) const {
    if (combo.key == ImGuiKey_None) return {};
    for (const auto& e : entries_) {
        if (e.name != exclude_name && e.combo == combo) return e.name;
    }
    return {};
}

std::string ShortcutManager::combo_to_string(const KeyCombo& combo) {
    if (combo.key == ImGuiKey_None) return {};
    std::string s;
    if (combo.ctrl)  s += "Ctrl+";
    if (combo.shift) s += "Shift+";
    if (combo.alt)   s += "Alt+";
    const char* key_name = ImGui::GetKeyName(combo.key);
    s += key_name ? key_name : "?";
    return s;
}

bool ShortcutManager::combo_from_string(const std::string& s, KeyCombo& out) {
    out = KeyCombo{};
    if (s.empty()) return false;

    std::istringstream ss(s);
    std::string token;
    std::string key_token;
    while (std::getline(ss, token, '+')) {
        // 大小写不敏感的修饰键匹配
        std::string lower;
        lower.reserve(token.size());
        for (char c : token) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "ctrl")       out.ctrl = true;
        else if (lower == "shift") out.shift = true;
        else if (lower == "alt")   out.alt = true;
        else                       key_token = token;
    }
    if (key_token.empty()) return false;

    // 反向查找 ImGuiKey：遍历已知键范围匹配名称
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const char* name = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
        if (name && key_token == name) {
            out.key = static_cast<ImGuiKey>(k);
            return true;
        }
    }
    return false;
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
    if (suspended_) return;
    ImGuiIO& io = ImGui::GetIO();
    // 文本输入框获焦时不触发全局快捷键
    if (io.WantTextInput) return;

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
