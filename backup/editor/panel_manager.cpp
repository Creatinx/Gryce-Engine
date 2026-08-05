#include "panel_manager.h"

#include <imgui_internal.h> // DockBuilder API（默认布局构建）

#include "localization/localization.h"
#include "fluent_window.h"

namespace gryce_engine::editor {

namespace {

// 在 DockSpace 节点树中递归定位中央节点（Viewport/Game 所在标签页）
ImGuiDockNode* find_central_node(ImGuiDockNode* node) {
    if (!node) return nullptr;
    if (node->IsCentralNode()) return node;
    if (node->ChildNodes[0]) {
        if (ImGuiDockNode* found = find_central_node(node->ChildNodes[0])) return found;
    }
    if (node->ChildNodes[1]) {
        if (ImGuiDockNode* found = find_central_node(node->ChildNodes[1])) return found;
    }
    return nullptr;
}

} // namespace

void PanelManager::show() {
    // 全屏宿主窗口：仅承载 DockSpace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    // 标题栏高度（FluentWindow 提供）
    const float title_bar_h = FluentWindow_GetTitleBarHeight();
    
    // DockSpace 从标题栏下方开始
    ImVec2 dock_pos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + title_bar_h);
    ImVec2 dock_size = ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - title_bar_h);
    
    ImGui::SetNextWindowPos(dock_pos);
    ImGui::SetNextWindowSize(dock_size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoNavFocus;

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

    ImGui::End();

    for (auto& panel : panels_) {
        panel->show();
    }

    // 首帧强制激活 Scene View（Viewport），避免从 imgui.ini 恢复时 Game View 抢占焦点。
    // 必须在所有面板窗口 show() 完成、Dock 节点状态更新后再设置 SelectedTabId。
    if (first_frame_) {
        first_frame_ = false;
        if (ImGuiDockNode* central = find_central_node(ImGui::DockBuilderGetNode(dockspace_id))) {
            central->SelectedTabId = ImGui::GetID("Viewport");
            central->LastFrameAlive = ImGui::GetFrameCount();
        }
    }
}

void PanelManager::build_default_layout(ImGuiID dockspace_id) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    // Unity Editor 风格默认布局：
    // 左 Hierarchy | 右 Inspector | 下 Project/Console 标签页 | 中 Viewport
    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.18f, nullptr, &dock_main);
    ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.22f, nullptr, &dock_main);
    ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.25f, nullptr, &dock_main);

    ImGui::DockBuilderDockWindow("Hierarchy", dock_left);
    ImGui::DockBuilderDockWindow("Inspector", dock_right);
    // File Explorer 与 Console 放在同一 dock 节点，以标签页形式并列（Unity 默认风格）
    ImGui::DockBuilderDockWindow("File Explorer", dock_bottom);
    ImGui::DockBuilderDockWindow("Console", dock_bottom);
    // Viewport 与 Game View 以标签页形式共享中间区域
    ImGui::DockBuilderDockWindow("Viewport", dock_main);
    ImGui::DockBuilderDockWindow("Game", dock_main);
    ImGui::DockBuilderFinish(dockspace_id);

    // DockBuilderFinish 之后再强制激活 Scene View（Viewport），
    // 否则 SelectedTabId 会被后续节点状态更新覆盖。
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock_main)) {
        node->SelectedTabId = ImGui::GetID("Viewport");
        node->LastFrameAlive = ImGui::GetFrameCount();
    }
}

} // namespace gryce_engine::editor
