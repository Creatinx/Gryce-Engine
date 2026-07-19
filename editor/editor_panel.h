#pragma once

#include <string>
#include <utility>

#include <imgui.h>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorPanel — 编辑器面板基类（M1-E1）
// 统一处理可见性开关与 ImGui Begin/End，派生类只需实现 on_imgui()。
// 面板名即 ImGui 窗口名，同时作为 DockBuilder 默认布局的 dock 目标。
// ---------------------------------------------------------------------------
class EditorPanel {
public:
    explicit EditorPanel(std::string name) : name_(std::move(name)) {}
    virtual ~EditorPanel() = default;

    EditorPanel(const EditorPanel&) = delete;
    EditorPanel& operator=(const EditorPanel&) = delete;

    const std::string& name() const { return name_; }
    bool visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }

    // 供 PanelManager 的 Window 菜单切换可见性
    bool* visible_ptr() { return &visible_; }

    // 每帧绘制：不可见时直接跳过
    void show() {
        if (!visible_) return;
        if (ImGui::Begin(name_.c_str(), &visible_)) {
            on_imgui();
        }
        ImGui::End();
    }

protected:
    virtual void on_imgui() = 0;

private:
    std::string name_;
    bool visible_ = true;
};

} // namespace gryce_engine::editor
