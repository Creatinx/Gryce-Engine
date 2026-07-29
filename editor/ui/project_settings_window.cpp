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

    int api = 0;
    switch (settings.render_api) {
        case render::RenderAPI::Vulkan: api = 0; break;
        case render::RenderAPI::OpenGL: api = 1; break;
        case render::RenderAPI::DX11:   api = 2; break;
        case render::RenderAPI::DX12:   api = 3; break;
    }
    const char* apis[] = {tr("project_settings.render_api_vulkan_default"),
                          tr("project_settings.render_api_opengl_compat"),
                          tr("project_settings.render_api_dx11_reserved"),
                          tr("project_settings.render_api_dx12_reserved")};
    if (ImGui::Combo(tr("project_settings.render_api"), &api, apis, IM_ARRAYSIZE(apis))) {
        if (api >= 2) {
            // DX11/12 为 WinNative 预留后端，尚未实现：不接受选择。
            dx_reserved_warn_ = true;
        } else {
            settings.render_api = (api == 1) ? render::RenderAPI::OpenGL : render::RenderAPI::Vulkan;
            unsaved_changes_ = true;
            dx_reserved_warn_ = false;
        }
    }
    if (dx_reserved_warn_) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "%s", tr("project_settings.backend_reserved"));
    }
    ImGui::TextDisabled("%s", tr("project_settings.restart_required"));

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Text("%s", tr("project_settings.quality"));
    ImGui::Separator();

    auto& q = settings.quality;

    // 阴影贴图分辨率
    const int sizes[] = {512, 1024, 2048, 4096};
    int size_idx = 1;
    for (int i = 0; i < 4; ++i) {
        if (sizes[i] == q.shadow_map_size) { size_idx = i; break; }
    }
    const char* size_names[] = {"512", "1024", "2048", "4096"};
    if (ImGui::Combo(tr("project_settings.shadow_map_size"), &size_idx, size_names, IM_ARRAYSIZE(size_names))) {
        q.shadow_map_size = sizes[size_idx];
        unsaved_changes_ = true;
    }

    if (ImGui::SliderFloat(tr("project_settings.shadow_bias"), &q.shadow_bias, 0.0f, 0.01f, "%.4f")) {
        unsaved_changes_ = true;
    }
    if (ImGui::SliderFloat(tr("project_settings.shadow_area"), &q.shadow_area, 5.0f, 100.0f, "%.1f")) {
        unsaved_changes_ = true;
    }
    if (ImGui::ColorEdit3(tr("project_settings.ambient"), q.ambient)) {
        unsaved_changes_ = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Checkbox(tr("project_settings.hdr_enabled"), &q.hdr_enabled)) {
        unsaved_changes_ = true;
    }
    if (q.hdr_enabled) {
        const char* tone_modes[] = {"None", "Reinhard", "ACES"};
        if (ImGui::Combo(tr("project_settings.tone_map_mode"), &q.tone_map_mode, tone_modes, IM_ARRAYSIZE(tone_modes))) {
            unsaved_changes_ = true;
        }
        if (ImGui::SliderFloat(tr("project_settings.exposure"), &q.exposure, 0.1f, 8.0f, "%.2f")) {
            unsaved_changes_ = true;
        }
    }
    if (ImGui::SliderFloat(tr("project_settings.ibl_intensity"), &q.ibl_intensity, 0.0f, 4.0f, "%.2f")) {
        unsaved_changes_ = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        save(project_root_, settings);
        unsaved_changes_ = false;
    }
}

} // namespace gryce_engine::editor
