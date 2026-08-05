#pragma once

#include <string>
#include <utility>

#include <imgui.h>

#include "localization/localization.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorPanel — 编辑器面板基类（M1-E1）
// 统一处理可见性开关与 ImGui Begin/End，派生类只需实现 on_imgui()。
// 面板名即 ImGui 窗口名，同时作为 DockBuilder 默认布局的 dock 目标。
// ---------------------------------------------------------------------------
class EditorPanel {
public:
    explicit EditorPanel(std::string name, std::string translation_key = {})
        : name_(std::move(name)), translation_key_(std::move(translation_key)) {}
    virtual ~EditorPanel() = default;

    EditorPanel(const EditorPanel&) = delete;
    EditorPanel& operator=(const EditorPanel&) = delete;

    const std::string& name() const { return name_; }
    const std::string& translation_key() const { return translation_key_; }
    bool visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }

    // 本帧是否真正绘制（窗口未折叠且是当前活动标签页）
    bool is_active() const { return drawn_this_frame_; }

    // 供 PanelManager 的 Window 菜单切换面板可见性
    bool* visible_ptr() { return &visible_; }

    // 每帧绘制：不可见时直接跳过
    // 窗口 ID 保持英文 name_（避免 dock 状态丢失），显示标题走本地化
    void show() {
        drawn_this_frame_ = false;
        if (!visible_) return;
        const char* display = translation_key_.empty() ? name_.c_str() : tr(translation_key_.c_str());
        std::string window_label = std::string(display) + "###" + name_;
        if (ImGui::Begin(window_label.c_str(), &visible_)) {
            drawn_this_frame_ = true;
            on_imgui();
        }
        ImGui::End();
    }

protected:
    virtual void on_imgui() = 0;

private:
    std::string name_;
    std::string translation_key_;
    bool visible_ = true;
    bool drawn_this_frame_ = false;
};

} // namespace gryce_engine::editor
