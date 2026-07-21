#include "project_settings_window.h"

#include <imgui.h>

namespace gryce_engine::editor {

ProjectSettings ProjectSettingsWindow::load(const std::string& project_root) {
    return load_project_settings(project_root);
}

void ProjectSettingsWindow::save(const std::string& project_root, const ProjectSettings& settings) {
    save_project_settings(project_root, settings);
}

bool ProjectSettingsWindow::draw(const std::string& project_root, ProjectSettings& settings) {
    if (!open_) return false;
    project_root_ = project_root;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("project_settings.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        const float sidebar_width = 140.0f;
        draw_sidebar(sidebar_width);

        ImGui::SameLine();
        ImGui::BeginChild("##project_settings_content", ImVec2(0.0f, 0.0f), true);
        switch (current_section_) {
            case Section::Graphics:
                draw_graphics_section(settings);
                break;
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
    }
    return open_;
}

void ProjectSettingsWindow::draw_sidebar(float width) {
    ImGui::BeginChild("##project_settings_sidebar", ImVec2(width, 0.0f), true);

    auto item = [&](Section section, const char* label) {
        bool selected = (current_section_ == section);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0.0f, 28.0f))) {
            current_section_ = section;
        }
    };

    item(Section::Graphics, tr("project_settings.section.graphics"));

    ImGui::EndChild();
}

void ProjectSettingsWindow::draw_graphics_section(ProjectSettings& settings) {
    ImGui::Text("%s", tr("project_settings.graphics"));
    ImGui::Separator();

    int api = (settings.render_api == render::RenderAPI::Vulkan) ? 1 : 0;
    const char* apis[] = {tr("project_settings.render_api_opengl"),
                          tr("project_settings.render_api_vulkan")};
    if (ImGui::Combo(tr("project_settings.render_api"), &api, apis, IM_ARRAYSIZE(apis))) {
        settings.render_api = (api == 1) ? render::RenderAPI::Vulkan : render::RenderAPI::OpenGL;
        unsaved_changes_ = true;
    }
    ImGui::TextDisabled("%s", tr("project_settings.restart_required"));

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        save(project_root_, settings);
        unsaved_changes_ = false;
    }
}

} // namespace gryce_engine::editor
