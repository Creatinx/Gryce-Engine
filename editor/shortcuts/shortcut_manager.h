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

    // 注册一个快捷键。同名会覆盖。
    void register_shortcut(const std::string& name, KeyCombo combo, Action action);

    // 取消注册
    void unregister_shortcut(const std::string& name);

    // 每帧调用；在 ImGui 键盘未捕获输入时检查触发
    void process();

    // 获取已注册快捷键列表（用于设置面板展示）
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> name_to_index_;

    bool combo_triggered(const KeyCombo& combo) const;
};

} // namespace gryce_engine::editor
