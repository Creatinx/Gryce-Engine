#include "gimport_editor_window.h"

#include <imgui.h>
#include <cstring>

#include "../localization/localization.h"

namespace gryce_engine::editor {

void GImportEditorWindow::open(const std::string& source_path) {
    source_path_ = source_path;
    settings_ = load_gimport_settings(source_path);
    std::strncpy(physics_material_buf_, settings_.physics_material.c_str(), sizeof(physics_material_buf_) - 1);
    physics_material_buf_[sizeof(physics_material_buf_) - 1] = '\0';
    open_ = true;
}

void GImportEditorWindow::draw() {
    if (!open_) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400.0f, 260.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("gimport_editor.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("%s: %s", tr("gimport_editor.source"), source_path_.c_str());
        ImGui::Separator();

        ImGui::DragFloat(tr("gimport_editor.scale"), &settings_.scale, 0.01f, 0.001f, 1000.0f);
        ImGui::Checkbox(tr("gimport_editor.generate_collider"), &settings_.generate_collider);
        ImGui::Checkbox(tr("gimport_editor.add_rigidbody"), &settings_.add_rigidbody);

        if (ImGui::InputText(tr("gimport_editor.physics_material"), physics_material_buf_, sizeof(physics_material_buf_))) {
            settings_.physics_material = physics_material_buf_;
        }
        ImGui::TextDisabled("%s", tr("gimport_editor.physics_material_hint"));

        ImGui::Separator();
        if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
            settings_.physics_material = physics_material_buf_;
            save_gimport_settings(source_path_, settings_);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("dialog.cancel"), ImVec2(100.0f, 0.0f))) {
            still_open = false;
        }
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
    }
}

} // namespace gryce_engine::editor
