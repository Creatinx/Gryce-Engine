#pragma once

#include <string>

#include <imgui.h>

#include "editor_theme.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// SettingsWindow — 编辑器设置窗口（File > Settings）
// ---------------------------------------------------------------------------
// 左侧栏目列表，右侧内容区。当前栏目：
//   - Theme：主题、强调色、字体、圆角、阴影
//   - Appliance：语言、（后续可扩展自动保存、启动行为等）
// ---------------------------------------------------------------------------

struct ApplianceSettings {
    Language language = Language::English;
};

struct EditorSettings {
    ThemeConfig theme;
    ThemePreset theme_preset = ThemePreset::Dark;
    ApplianceSettings appliance;
};

class SettingsWindow {
public:
    // 尝试从项目根目录加载 editor_settings.json 与 editor_theme.json；
    // 失败则返回默认设置。
    static EditorSettings load(const std::string& project_root);

    // 保存当前设置到项目根目录。
    static void save(const std::string& project_root, const EditorSettings& settings);

    // 绘制窗口。若窗口仍打开返回 true，关闭后返回 false。
    bool draw(const std::string& project_root, EditorSettings& settings);

    void open() { open_ = true; }
    bool is_open() const { return open_; }

    // 若字体大小已停止变化并需要重建 atlas，返回 true 并消费该请求。
    // 由 editor_app.cpp 在合适的时机（渲染线程暂停、持有 GPU context）处理。
    bool consume_font_rebuild_ready();

private:
    enum class Section { Theme, Appliance };

    void draw_sidebar(float width);
    void draw_theme_section(EditorSettings& settings);
    void draw_appliance_section(EditorSettings& settings);
    void apply_theme_live(const EditorSettings& settings);
    void apply_and_save(const std::string& project_root, EditorSettings& settings);
    void flush_save(const std::string& project_root, EditorSettings& settings);

    bool open_ = false;
    Section current_section_ = Section::Theme;
    bool unsaved_changes_ = false;
    std::string project_root_;
    float save_debounce_ = 0.0f;
    float last_font_size_ = 0.0f;
    bool font_rebuild_pending_ = false;
    bool font_rebuild_ready_ = false;
};

const char* language_name(Language lang);

} // namespace gryce_engine::editor
