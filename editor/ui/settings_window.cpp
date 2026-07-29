#include "settings_window.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "render/render_context.h"
#include "utils/glog/glog_lib.h"
#include "../shortcuts/shortcut_manager.h"

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
        if (j.contains("editor")) {
            const auto& ed = j["editor"];
            settings.editor.vsync = ed.value("vsync", true);
            settings.editor.autosave_interval_min = ed.value("autosave_interval_min", 5);
        }
        if (j.contains("shortcuts") && j["shortcuts"].is_object()) {
            for (auto it = j["shortcuts"].begin(); it != j["shortcuts"].end(); ++it) {
                if (it.value().is_string()) {
                    settings.shortcut_overrides[it.key()] = it.value().get<std::string>();
                }
            }
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
    j["editor"]["vsync"] = settings.editor.vsync;
    j["editor"]["autosave_interval_min"] = settings.editor.autosave_interval_min;
    for (const auto& [name, combo] : settings.shortcut_overrides) {
        j["shortcuts"][name] = combo;
    }

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
    if (!open_) {
        // 窗口关闭时确保快捷键不处于挂起状态
        if (shortcut_mgr_ && shortcut_mgr_->suspended()) {
            shortcut_mgr_->set_suspended(false);
            rebinding_shortcut_.clear();
        }
        return false;
    }
    project_root_ = project_root;

    // 捕获新按键期间挂起全局快捷键，避免误触发
    if (shortcut_mgr_) {
        shortcut_mgr_->set_suspended(!rebinding_shortcut_.empty());
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("settings.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        const float sidebar_width = 160.0f;
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
            case Section::Editor:
                draw_editor_section(settings);
                break;
            case Section::Shortcuts:
                draw_shortcuts_section(settings);
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
    item(Section::Editor, tr("settings.section.editor"));
    item(Section::Shortcuts, tr("settings.section.shortcuts"));

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

void SettingsWindow::draw_editor_section(EditorSettings& settings) {
    ImGui::Text("%s", tr("settings.section.editor"));
    ImGui::Separator();

    // VSync：立即生效并持久化
    if (ImGui::Checkbox(tr("settings.vsync"), &settings.editor.vsync)) {
        if (render_ctx_) {
            render_ctx_->set_swap_interval(settings.editor.vsync ? 1 : 0);
        }
        unsaved_changes_ = true;
        save_debounce_ = 0.5f;
    }
    ImGui::TextDisabled("%s", tr("settings.vsync_hint"));

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Text("%s", tr("settings.autosave_interval"));
    if (ImGui::SliderInt("##autosave_interval", &settings.editor.autosave_interval_min, 0, 30, "%d min")) {
        settings.editor.autosave_interval_min = std::clamp(settings.editor.autosave_interval_min, 0, 30);
        unsaved_changes_ = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        save_debounce_ = 0.5f;
    }
    if (settings.editor.autosave_interval_min == 0) {
        ImGui::TextDisabled("%s", tr("settings.autosave_disabled"));
    }

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        flush_save(project_root_, settings);
    }
}

void SettingsWindow::draw_shortcuts_section(EditorSettings& settings) {
    ImGui::Text("%s", tr("settings.section.shortcuts"));
    ImGui::Separator();

    if (!shortcut_mgr_) {
        ImGui::TextDisabled("%s", tr("settings.shortcuts.unavailable"));
        return;
    }

    // 捕获模式：下一个按下的键（含当前修饰键）成为新组合键，Esc 取消
    if (!rebinding_shortcut_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                           tr("settings.shortcuts.press_key"));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            rebinding_shortcut_.clear();
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                ImGuiKey key = static_cast<ImGuiKey>(k);
                // 跳过纯修饰键本身
                if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
                    key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                    key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) {
                    continue;
                }
                if (ImGui::IsKeyPressed(key, false)) {
                    ShortcutManager::KeyCombo combo;
                    combo.key = key;
                    const ImGuiIO& io = ImGui::GetIO();
                    combo.ctrl = io.KeyCtrl;
                    combo.shift = io.KeyShift;
                    combo.alt = io.KeyAlt;
                    std::string conflict = shortcut_mgr_->conflict_of(combo, rebinding_shortcut_);
                    if (!conflict.empty()) {
                        rebind_conflict_ = conflict;
                    } else {
                        shortcut_mgr_->set_combo(rebinding_shortcut_, combo);
                        rebind_conflict_.clear();
                        unsaved_changes_ = true;
                    }
                    rebinding_shortcut_.clear();
                    break;
                }
            }
        }
        ImGui::Separator();
    }
    if (!rebind_conflict_.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "%s: %s",
                           tr("settings.shortcuts.conflict"), rebind_conflict_.c_str());
    }

    // 快捷键列表
    if (ImGui::BeginTable("##shortcuts_table", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##combo", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        for (const auto& entry : shortcut_mgr_->entries()) {
            ImGui::PushID(entry.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ShortcutManager::combo_to_string(entry.combo).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr("settings.shortcuts.rebind"))) {
                rebinding_shortcut_ = entry.name;
                rebind_conflict_.clear();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("settings.shortcuts.reset"))) {
                shortcut_mgr_->reset_to_default(entry.name);
                unsaved_changes_ = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Button(tr("settings.shortcuts.reset_all"), ImVec2(140.0f, 0.0f))) {
        shortcut_mgr_->reset_all_defaults();
        unsaved_changes_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("settings.apply"), ImVec2(100.0f, 0.0f))) {
        flush_save(project_root_, settings);
    }
}

void SettingsWindow::apply_theme_live(const EditorSettings& settings) {
    Localization::instance().set_light_theme(settings.theme_preset == ThemePreset::Light);
    apply_theme(settings.theme_preset, settings.theme);
}

void SettingsWindow::flush_save(const std::string& project_root, EditorSettings& settings) {
    // 把当前快捷键组合同步进待保存设置
    if (shortcut_mgr_) {
        settings.shortcut_overrides.clear();
        for (const auto& entry : shortcut_mgr_->entries()) {
            settings.shortcut_overrides[entry.name] = ShortcutManager::combo_to_string(entry.combo);
        }
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
