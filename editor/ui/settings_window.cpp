#include "settings_window.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

std::string settings_json_path(const std::string& project_root) {
    return project_root + "/editor_settings.json";
}

std::string language_to_string(Language lang) {
    return language_code(lang);
}

Language language_from_string(const std::string& s) {
    if (s == "zh") return Language::Chinese;
    return Language::English;
}

} // namespace

const char* language_name(Language lang) {
    return language_display_name(lang);
}

EditorSettings SettingsWindow::load(const std::string& project_root) {
    EditorSettings settings;

    // 主题配置沿用 editor_theme.json
    ThemeConfig theme_cfg;
    ThemePreset theme_preset;
    if (load_theme_config(project_root, theme_cfg, theme_preset)) {
        settings.theme = theme_cfg;
        settings.theme_preset = theme_preset;
        settings.ui_scale = settings.theme.ui_scale;
    }

    // 通用配置从 editor_settings.json 读取
    std::string path = settings_json_path(project_root);
    if (!std::filesystem::exists(path)) {
        return settings;
    }
    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("SettingsWindow: failed to open '{}'", path);
        return settings;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("appliance")) {
            const auto& app = j["appliance"];
            settings.appliance.language = language_from_string(app.value("language", "en"));
        }
    } catch (const std::exception& e) {
        GLOG_ERROR("SettingsWindow: failed to parse '{}': {}", path, e.what());
    }
    return settings;
}

void SettingsWindow::save(const std::string& project_root, const EditorSettings& settings) {
    // 主题单独保持兼容 editor_theme.json
    EditorSettings persisted = settings;
    persisted.theme.ui_scale = settings.ui_scale;
    save_theme_config(project_root, persisted.theme, persisted.theme_preset);

    // 通用配置写入 editor_settings.json
    nlohmann::json j;
    j["appliance"]["language"] = language_to_string(settings.appliance.language);

    std::string path = settings_json_path(project_root);
    std::ofstream ofs(path);
    if (!ofs) {
        GLOG_ERROR("SettingsWindow: failed to write '{}'", path);
        return;
    }
    ofs << j.dump(4);
    GLOG_INFO("SettingsWindow: saved settings '{}'", path);
}

bool SettingsWindow::draw(const std::string& project_root, EditorSettings& settings) {
    if (!open_) return false;
    project_root_ = project_root;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("settings.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        const float sidebar_width = 140.0f;
        draw_sidebar(sidebar_width);

        ImGui::SameLine();
        ImGui::BeginChild("##settings_content", ImVec2(0.0f, 0.0f), true);
        switch (current_section_) {
            case Section::Theme:
                draw_theme_section(settings);
                break;
            case Section::Appliance:
                draw_appliance_section(settings);
                break;
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // 延迟保存 + 字体热重载防抖：用户停止拖动 0.5s 后再真正保存/请求重建。
    const float dt = ImGui::GetIO().DeltaTime;
    if (save_debounce_ > 0.0f) {
        save_debounce_ -= dt;
        if (save_debounce_ <= 0.0f) {
            flush_save(project_root, settings);
        }
    }

    if (!still_open) {
        if (unsaved_changes_) {
            flush_save(project_root, settings);
        }
        open_ = false;
    }
    return open_;
}

void SettingsWindow::draw_sidebar(float width) {
    ImGui::BeginChild("##settings_sidebar", ImVec2(width, 0.0f), true);

    auto item = [&](Section section, const char* label) {
        bool selected = (current_section_ == section);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0.0f, 28.0f))) {
            current_section_ = section;
        }
    };

    item(Section::Theme, tr("settings.section.theme"));
    item(Section::Appliance, tr("settings.section.appliance"));

    ImGui::EndChild();
}

void SettingsWindow::draw_theme_section(EditorSettings& settings) {
    ImGui::Text("%s", tr("settings.appearance"));
    ImGui::Separator();

    int preset = 0;
    if (settings.theme_preset == ThemePreset::Light)       preset = 1;
    else if (settings.theme_preset == ThemePreset::ModernLight) preset = 2;
    const char* presets[] = {tr("menu.view_theme_dark"), tr("menu.view_theme_light"), tr("menu.view_theme_modern_light")};
    if (ImGui::Combo(tr("settings.theme_preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
        if (preset == 0)       settings.theme_preset = ThemePreset::Dark;
        else if (preset == 1)  settings.theme_preset = ThemePreset::Light;
        else                   settings.theme_preset = ThemePreset::ModernLight;
        apply_theme_live(settings);
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    bool scale_changed = ImGui::SliderFloat(tr("settings.ui_scale"), &settings.ui_scale, 1.0f, 2.0f, "%.2fx");
    if (scale_changed) {
        settings.ui_scale = std::clamp(settings.ui_scale, 1.0f, 2.0f);
        settings.theme.ui_scale = settings.ui_scale;
        unsaved_changes_ = true;
    }
    if (scale_changed && ImGui::IsItemDeactivatedAfterEdit()) {
        apply_theme_live(settings);
        save_debounce_ = 0.5f;
    }

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        flush_save(project_root_, settings);
    }
}

void SettingsWindow::draw_appliance_section(EditorSettings& settings) {
    ImGui::Text("%s", tr("settings.section.appliance"));
    ImGui::Separator();

    int lang = static_cast<int>(settings.appliance.language);
    const char* languages[] = {language_display_name(Language::English),
                               language_display_name(Language::Chinese)};
    if (ImGui::Combo(tr("settings.language"), &lang, languages, IM_ARRAYSIZE(languages))) {
        settings.appliance.language = static_cast<Language>(lang);
        unsaved_changes_ = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        apply_and_save(project_root_, settings);
    }
}

void SettingsWindow::apply_theme_live(const EditorSettings& settings) {
    Localization::instance().set_light_theme(settings.theme_preset == ThemePreset::Light);
    apply_theme(settings.theme_preset, settings.theme);
}

void SettingsWindow::flush_save(const std::string& project_root, EditorSettings& settings) {
    Localization::instance().load(settings.appliance.language, project_root);
    save(project_root, settings);
    unsaved_changes_ = false;
    save_debounce_ = 0.0f;
}

void SettingsWindow::apply_and_save(const std::string& project_root, EditorSettings& settings) {
    // 先切换语言，再应用主题/字体，确保 CJK 字体按需合并
    Localization::instance().load(settings.appliance.language, project_root);
    Localization::instance().set_light_theme(settings.theme_preset == ThemePreset::Light);
    apply_theme(settings.theme_preset, settings.theme);
    save(project_root, settings);
    unsaved_changes_ = false;
}

} // namespace gryce_engine::editor
