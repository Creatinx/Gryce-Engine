#include "panel_manager.h"

#include <imgui_internal.h> // DockBuilder API（默认布局构建）

#include "localization/localization.h"

namespace gryce_engine::editor {

void PanelManager::show() {
    // 全屏宿主窗口：仅承载 DockSpace 与菜单栏
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##EditorDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    // 首跑检测：DockSpace 节点不存在说明 ini 中没有布局记录，构建默认布局
    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    if (!layout_checked_) {
        layout_checked_ = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            build_default_layout(dockspace_id);
        }
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // 菜单栏：应用菜单钩子（File 等） + Window 菜单切换面板可见性
    if (ImGui::BeginMenuBar()) {
        if (menu_bar_hook_) {
            menu_bar_hook_();
        }
        if (ImGui::BeginMenu(tr("menu.window"))) {
            for (auto& panel : panels_) {
                const char* display = panel->translation_key().empty()
                                          ? panel->name().c_str()
                                          : tr(panel->translation_key().c_str());
                ImGui::MenuItem(display, nullptr, panel->visible_ptr());
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();

    for (auto& panel : panels_) {
        panel->show();
    }
}

void PanelManager::build_default_layout(ImGuiID dockspace_id) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    // 左 Hierarchy | 右 Inspector | 下 Console + Project | 中 Viewport
    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.20f, nullptr, &dock_main);
    ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.25f, nullptr, &dock_main);
    ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.28f, nullptr, &dock_main);
    ImGuiID dock_bottom_right = ImGui::DockBuilderSplitNode(dock_bottom, ImGuiDir_Right, 0.40f, nullptr, &dock_bottom);

    ImGui::DockBuilderDockWindow("Hierarchy", dock_left);
    ImGui::DockBuilderDockWindow("Inspector", dock_right);
    ImGui::DockBuilderDockWindow("Console", dock_bottom);
    ImGui::DockBuilderDockWindow("Project", dock_bottom_right);
    ImGui::DockBuilderDockWindow("Viewport", dock_main);

    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace gryce_engine::editor
