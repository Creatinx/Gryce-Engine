#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ShortcutManager — 编辑器全局快捷键管理
// 支持修饰键组合（Ctrl/Shift/Alt）+ ImGuiKey，绑定到无参动作。
// 在主循环 ImGui 帧之间调用 process() 即可；自动跳过文本输入框获焦时。
// ---------------------------------------------------------------------------
class ShortcutManager {
public:
    using Action = std::function<void()>;

    struct KeyCombo {
        ImGuiKey key = ImGuiKey_None;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;

        bool operator==(const KeyCombo& other) const {
            return key == other.key && ctrl == other.ctrl &&
                   shift == other.shift && alt == other.alt;
        }
    };

    struct Entry {
        KeyCombo combo;
        Action action;
        std::string name;
    };

    ShortcutManager() = default;
    ~ShortcutManager() = default;

    // 注册一个快捷键。同名会覆盖（action 更新，default 保持首次注册值）。
    void register_shortcut(const std::string& name, KeyCombo combo, Action action);

    // 取消注册
    void unregister_shortcut(const std::string& name);

    // 每帧调用；在 ImGui 键盘未捕获输入时检查触发
    void process();

    // 挂起/恢复触发（设置面板捕获新按键时使用，避免误触发已有快捷键）
    void set_suspended(bool suspended) { suspended_ = suspended; }
    bool suspended() const { return suspended_; }

    // 获取已注册快捷键列表（用于设置面板展示）
    const std::vector<Entry>& entries() const { return entries_; }

    // 重绑定：修改指定快捷键的组合键。名称不存在返回 false。
    bool set_combo(const std::string& name, const KeyCombo& combo);
    // 查询当前组合键。名称不存在返回 false。
    bool combo_by_name(const std::string& name, KeyCombo& out) const;
    // 恢复首次注册时的默认组合键。
    bool reset_to_default(const std::string& name);
    void reset_all_defaults();
    // 冲突检测：返回占用该组合键的快捷键名称，无冲突返回空串。
    std::string conflict_of(const KeyCombo& combo, const std::string& exclude_name) const;

    // 组合键 <-> 字符串（"Ctrl+Shift+Z"），用于持久化与展示
    static std::string combo_to_string(const KeyCombo& combo);
    static bool combo_from_string(const std::string& s, KeyCombo& out);

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> name_to_index_;
    std::unordered_map<std::string, KeyCombo> defaults_; // 首次注册的组合键

    bool suspended_ = false;

    bool combo_triggered(const KeyCombo& combo) const;
};

} // namespace gryce_engine::editor
