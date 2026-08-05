#include "settings_window.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "render/render_context.h"
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

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 栏目树：分组 › 页面（只包含真实存在的设置页）
const SettingsWindow::PageInfo k_pages[] = {
    {SettingsWindow::Page::Appearance,    "settings.page.appearance", "settings.group.appearance_behavior"},
    {SettingsWindow::Page::Language,      "settings.page.language",   "settings.group.appearance_behavior"},
    {SettingsWindow::Page::EditorGeneral, "settings.page.general",    "settings.group.editor"},
    {SettingsWindow::Page::Shortcuts,     "settings.section.shortcuts", nullptr},
};

const char* k_groups[] = {
    "settings.group.appearance_behavior",
    "settings.group.editor",
};

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

// ---------------------------------------------------------------------------
// 主绘制
// ---------------------------------------------------------------------------
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

    // 打开时快照：设置副本（暂存编辑）+ 快捷键组合（取消时还原）
    if (just_opened_) {
        just_opened_ = false;
        staged_ = settings;
        dirty_ = false;
        search_buf_[0] = '\0';
        rebinding_shortcut_.clear();
        rebind_conflict_.clear();
        shortcut_snapshot_.clear();
        if (shortcut_mgr_) {
            for (const auto& entry : shortcut_mgr_->entries()) {
                shortcut_snapshot_.emplace_back(entry.name, entry.combo);
            }
        }
    }

    // 捕获新按键期间挂起全局快捷键，避免误触发
    if (shortcut_mgr_) {
        shortcut_mgr_->set_suspended(!rebinding_shortcut_.empty());
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(880.0f, 560.0f), ImGuiCond_Appearing);

    const PageInfo* current_info = &k_pages[0];
    for (const auto& p : k_pages) {
        if (p.page == current_page_) {
            current_info = &p;
            break;
        }
    }

    bool still_open = true;
    if (ImGui::Begin(tr("settings.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        const float footer_h = ImGui::GetFrameHeightWithSpacing() + 12.0f;
        const float region_h = ImGui::GetContentRegionAvail().y - footer_h;

        // 左侧栏目树
        ImGui::BeginChild("##settings_sidebar", ImVec2(240.0f, region_h), false);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##settings_search", tr("settings.search_hint"),
                                 search_buf_, sizeof(search_buf_));
        ImGui::Spacing();
        draw_sidebar_tree();
        ImGui::EndChild();

        // 1px 竖直分隔线（JetBrains 风格 hairline divider，替代宽间距）
        ImGui::SameLine(0.0f, 6.0f);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x, p.y + region_h),
                                                ImGui::GetColorU32(ImGuiCol_Border), 1.0f);
        }
        ImGui::Dummy(ImVec2(1.0f, region_h));
        ImGui::SameLine(0.0f, 6.0f);

        // 右侧内容区：面包屑 + 当前页
        ImGui::BeginChild("##settings_content", ImVec2(0.0f, region_h), false);
        draw_breadcrumb(*current_info);
        ImGui::Separator();
        ImGui::Spacing();
        switch (current_page_) {
            case Page::Appearance:    draw_appearance_page();    break;
            case Page::Language:      draw_language_page();      break;
            case Page::EditorGeneral: draw_editor_general_page(); break;
            case Page::Shortcuts:     draw_shortcuts_page();     break;
        }
        ImGui::EndChild();

        // 底部按钮行（右对齐）
        ImGui::Separator();
        draw_footer_buttons(settings);
    }
    ImGui::End();

    if (!still_open) {
        cancel_and_close();
    }
    return open_;
}

// ---------------------------------------------------------------------------
// 左侧栏目树（搜索过滤）
// ---------------------------------------------------------------------------
bool SettingsWindow::page_matches_filter(const PageInfo& info) const {
    if (search_buf_[0] == '\0') return true;
    const std::string needle = to_lower(search_buf_);
    return to_lower(tr(info.name_key)).find(needle) != std::string::npos;
}

bool SettingsWindow::group_matches_filter(const char* group_key) const {
    if (search_buf_[0] == '\0') return true;
    const std::string needle = to_lower(search_buf_);
    if (to_lower(tr(group_key)).find(needle) != std::string::npos) return true;
    // 分组下任一页面匹配则分组可见
    for (const auto& p : k_pages) {
        if (p.group_key && std::string(p.group_key) == group_key && page_matches_filter(p)) {
            return true;
        }
    }
    return false;
}

void SettingsWindow::draw_sidebar_tree() {
    // 选中行用 Xcode 蓝高亮（JetBrains Settings 风格）
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.039f, 0.518f, 1.0f, 0.55f));

    // 分组节点（可展开/折叠；搜索时强制展开）
    for (const char* group_key : k_groups) {
        if (!group_matches_filter(group_key)) continue;

        const bool searching = search_buf_[0] != '\0';
        if (searching) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once); // 默认展开（JetBrains 风格）
        }
        if (!ImGui::TreeNode(tr(group_key))) continue;

        for (const auto& p : k_pages) {
            if (!p.group_key || std::string(p.group_key) != group_key) continue;
            if (!page_matches_filter(p)) continue;
            const bool selected = (current_page_ == p.page);
            if (ImGui::Selectable(tr(p.name_key), selected,
                                  ImGuiSelectableFlags_None, ImVec2(0.0f, 26.0f))) {
                current_page_ = p.page;
            }
        }
        ImGui::TreePop();
    }

    // 顶层叶子页（无分组）
    for (const auto& p : k_pages) {
        if (p.group_key) continue;
        if (!page_matches_filter(p)) continue;
        const bool selected = (current_page_ == p.page);
        if (ImGui::Selectable(tr(p.name_key), selected,
                              ImGuiSelectableFlags_None, ImVec2(0.0f, 26.0f))) {
            current_page_ = p.page;
        }
    }

    ImGui::PopStyleColor();
}

void SettingsWindow::draw_breadcrumb(const PageInfo& info) {
    if (info.group_key) {
        ImGui::TextDisabled("%s", tr(info.group_key));
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextDisabled("›");
        ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::TextUnformatted(tr(info.name_key));
}

// ---------------------------------------------------------------------------
// 底部按钮：确定 / 取消 / 应用
// ---------------------------------------------------------------------------
void SettingsWindow::draw_footer_buttons(EditorSettings& settings) {
    const float btn_w = 90.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total = btn_w * 3.0f + spacing * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - total);

    if (ImGui::Button(tr("common.ok"), ImVec2(btn_w, 0.0f))) {
        commit(settings);
        open_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("common.cancel"), ImVec2(btn_w, 0.0f))) {
        cancel_and_close();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button(tr("settings.apply"), ImVec2(btn_w, 0.0f))) {
        commit(settings);
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// 提交 / 取消
// ---------------------------------------------------------------------------
void SettingsWindow::commit(EditorSettings& settings) {
    staged_.theme.ui_scale = staged_.ui_scale;
    settings = staged_;

    // 生效：先切语言，再应用主题/字体（确保 CJK 字体按需合并），最后 VSync
    // 暂停渲染线程避免字体热重载导致 GPU 纹理竞争
    if (render_ctx_) render_ctx_->pause_render_thread();
    Localization::instance().load(settings.appliance.language, project_root_);
    Localization::instance().set_light_theme(settings.theme_preset == ThemePreset::Light);
    apply_theme(settings.theme_preset, settings.theme);
    if (rebuild_fonts_fn_) rebuild_fonts_fn_();
    if (render_ctx_) {
        render_ctx_->resume_render_thread();
        render_ctx_->set_swap_interval(settings.editor.vsync ? 1 : 0);
    }

    // 快捷键组合同步进待保存设置
    if (shortcut_mgr_) {
        settings.shortcut_overrides.clear();
        for (const auto& entry : shortcut_mgr_->entries()) {
            settings.shortcut_overrides[entry.name] = ShortcutManager::combo_to_string(entry.combo);
        }
    }

    save(project_root_, settings);
    // 提交后副本与正式设置一致
    staged_ = settings;
    dirty_ = false;
}

void SettingsWindow::cancel_and_close() {
    // 还原打开窗口时的快捷键快照（重绑定/重置直接作用于 ShortcutManager，需手动回滚）
    if (shortcut_mgr_ && !shortcut_snapshot_.empty()) {
        for (const auto& [name, combo] : shortcut_snapshot_) {
            shortcut_mgr_->set_combo(name, combo);
        }
    }
    rebinding_shortcut_.clear();
    open_ = false;
}

// ---------------------------------------------------------------------------
// 页面：外观与行为 › 外观（主题预设 + UI 缩放）
// ---------------------------------------------------------------------------
void SettingsWindow::draw_appearance_page() {
    int preset = 0;
    if (staged_.theme_preset == ThemePreset::Light)       preset = 1;
    const char* presets[] = {tr("menu.view_theme_dark"), tr("menu.view_theme_light")};
    if (ImGui::Combo(tr("settings.theme_preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
        if (preset == 0)       staged_.theme_preset = ThemePreset::Dark;
        else                   staged_.theme_preset = ThemePreset::Light;
        dirty_ = true;
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::SliderFloat(tr("settings.ui_scale"), &staged_.ui_scale, 1.0f, 2.0f, "%.2fx")) {
        staged_.ui_scale = std::clamp(staged_.ui_scale, 1.0f, 2.0f);
        staged_.theme.ui_scale = staged_.ui_scale;
        dirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// 页面：外观与行为 › 语言和区域
// ---------------------------------------------------------------------------
void SettingsWindow::draw_language_page() {
    int lang = static_cast<int>(staged_.appliance.language);
    const char* languages[] = {language_display_name(Language::English),
                               language_display_name(Language::Chinese)};
    if (ImGui::Combo(tr("settings.language"), &lang, languages, IM_ARRAYSIZE(languages))) {
        staged_.appliance.language = static_cast<Language>(lang);
        dirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// 页面：编辑器 › 常规（VSync + 自动保存）
// ---------------------------------------------------------------------------
void SettingsWindow::draw_editor_general_page() {
    if (ImGui::Checkbox(tr("settings.vsync"), &staged_.editor.vsync)) {
        dirty_ = true;
    }
    ImGui::TextDisabled("%s", tr("settings.vsync_hint"));

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Text("%s", tr("settings.autosave_interval"));
    if (ImGui::SliderInt("##autosave_interval", &staged_.editor.autosave_interval_min, 0, 30, "%d min")) {
        staged_.editor.autosave_interval_min = std::clamp(staged_.editor.autosave_interval_min, 0, 30);
        dirty_ = true;
    }
    if (staged_.editor.autosave_interval_min == 0) {
        ImGui::TextDisabled("%s", tr("settings.autosave_disabled"));
    }
}

// ---------------------------------------------------------------------------
// 页面：快捷键（重绑定直接作用于 ShortcutManager；取消时整体还原快照）
// ---------------------------------------------------------------------------
void SettingsWindow::draw_shortcuts_page() {
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
                        dirty_ = true;
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
                dirty_ = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::Button(tr("settings.shortcuts.reset_all"), ImVec2(140.0f, 0.0f))) {
        shortcut_mgr_->reset_all_defaults();
        dirty_ = true;
    }
}

} // namespace gryce_engine::editor
