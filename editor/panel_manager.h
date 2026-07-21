#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "editor_panel.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// PanelManager — 编辑器面板管理（M1-E1）
// 负责：全屏 DockSpace（DockSpace over viewport）、首跑默认布局
// （DockBuilder 构建；此后布局由 imgui.ini 持久化）、Window 菜单可见性切换。
// 默认布局：Unity Editor 风格，左 Hierarchy、右 Inspector、下 Project/Console 标签页、中间 Viewport。
// File 等应用菜单通过 set_menu_bar_hook 注入（M1-E2 场景保存/加载）。
// ---------------------------------------------------------------------------
class PanelManager {
public:
    PanelManager() = default;
    ~PanelManager() = default;

    PanelManager(const PanelManager&) = delete;
    PanelManager& operator=(const PanelManager&) = delete;

    // 注册面板并返回原始指针（PanelManager 持有所有权）
    template<typename T, typename... Args>
    T* add_panel(Args&&... args) {
        auto panel = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = panel.get();
        panels_.push_back(std::move(panel));
        return ptr;
    }

    // 菜单栏扩展钩子：在 Window 菜单之前调用（EditorApp 注入 File 菜单）
    void set_menu_bar_hook(std::function<void()> hook) { menu_bar_hook_ = std::move(hook); }

    // 每帧绘制：DockSpace 宿主窗口 + 菜单 + 所有面板
    void show();

private:
    // 首次运行（无 ini 布局记录）时构建默认布局
    void build_default_layout(ImGuiID dockspace_id);

    std::vector<std::unique_ptr<EditorPanel>> panels_;
    std::function<void()> menu_bar_hook_;
    bool layout_checked_ = false;
};

} // namespace gryce_engine::editor
