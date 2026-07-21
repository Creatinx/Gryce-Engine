#include "settings_window.h"

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
    save_theme_config(project_root, settings.theme, settings.theme_preset);

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
        if (unsaved_changes_ || font_rebuild_pending_) {
            flush_save(project_root, settings);
        }
        open_ = false;
    }
    return open_;
}

bool SettingsWindow::consume_font_rebuild_ready() {
    const bool ready = font_rebuild_ready_;
    font_rebuild_ready_ = false;
    return ready;
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

    int preset = (settings.theme_preset == ThemePreset::Dark) ? 0 : 1;
    const char* presets[] = {tr("menu.view_theme_dark"), tr("menu.view_theme_light")};
    if (ImGui::Combo(tr("settings.theme_preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
        settings.theme_preset = (preset == 0) ? ThemePreset::Dark : ThemePreset::Light;
        apply_theme_live(settings);
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::SliderFloat(tr("settings.accent_hue"), &settings.theme.accent_hue, 0.0f, 1.0f)) {
        apply_theme_live(settings);
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }

    if (ImGui::SliderFloat(tr("settings.rounding"), &settings.theme.rounding, 0.0f, 12.0f)) {
        apply_theme_live(settings);
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }

    if (ImGui::SliderFloat(tr("settings.font_size"), &settings.theme.font_size, 12.0f, 32.0f)) {
        // 字体大小变化只做标记；真正 Clear/Build atlas 放到 editor_app.cpp 中
        // 在渲染线程暂停、持有 GPU context 时执行，避免帧中间操作 GPU 资源。
        font_rebuild_pending_ = true;
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }

    if (ImGui::Checkbox(tr("settings.shadow"), &settings.theme.shadow)) {
        apply_theme_live(settings);
        unsaved_changes_ = true;
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
    // 字体大小由 editor_app.cpp 在合适的时机（渲染线程暂停）统一重建 atlas。
    if (font_rebuild_pending_) {
        font_rebuild_ready_ = true;
        font_rebuild_pending_ = false;
    }
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
