#pragma once

#include <map>
#include <string>

#include <imgui.h>

#include "editor_theme.h"
#include "../localization/localization.h"

namespace gryce_engine::render { class RenderContext; }

namespace gryce_engine::editor {

class ShortcutManager;

// ---------------------------------------------------------------------------
// SettingsWindow — 编辑器设置窗口（File > Settings）
// ---------------------------------------------------------------------------
// 左侧栏目列表，右侧内容区。当前栏目：
//   - Theme：主题预设、UI 缩放
//   - Appliance：语言
//   - Editor：VSync、场景自动保存间隔
// ---------------------------------------------------------------------------

struct ApplianceSettings {
    Language language = Language::English;
};

// 编辑器行为设置（持久化到 editor_settings.json 的 "editor" 组）
struct EditorBehaviorSettings {
    bool vsync = true;                 // 垂直同步（立即生效并持久化）
    int autosave_interval_min = 5;     // 场景自动保存间隔（分钟），0 = 关闭
};

struct EditorSettings {
    ThemeConfig theme;
    ThemePreset theme_preset = ThemePreset::Dark;
    ApplianceSettings appliance;
    EditorBehaviorSettings editor;
    // 快捷键覆盖：name -> "Ctrl+Z" 形式的组合键字符串（启动时应用到 ShortcutManager）
    std::map<std::string, std::string> shortcut_overrides;
    // UI 全局缩放（与 theme.ui_scale 保持同步，方便设置界面绑定）
    float ui_scale = EngineTheme::k_default_ui_scale;
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

    // 注入渲染上下文：VSync 勾选时立即调用 set_swap_interval。
    void set_render_context(render::RenderContext* ctx) { render_ctx_ = ctx; }

    // 注入快捷键管理器：快捷键栏目的展示与重绑定。
    void set_shortcut_manager(ShortcutManager* mgr) { shortcut_mgr_ = mgr; }

    void open() { open_ = true; }
    bool is_open() const { return open_; }

private:
    enum class Section { Theme, Appliance, Editor, Shortcuts };

    void draw_sidebar(float width);
    void draw_theme_section(EditorSettings& settings);
    void draw_appliance_section(EditorSettings& settings);
    void draw_editor_section(EditorSettings& settings);
    void draw_shortcuts_section(EditorSettings& settings);
    void apply_theme_live(const EditorSettings& settings);
    void apply_and_save(const std::string& project_root, EditorSettings& settings);
    void flush_save(const std::string& project_root, EditorSettings& settings);

    bool open_ = false;
    Section current_section_ = Section::Theme;
    bool unsaved_changes_ = false;
    std::string project_root_;
    float save_debounce_ = 0.0f;
    render::RenderContext* render_ctx_ = nullptr;
    ShortcutManager* shortcut_mgr_ = nullptr;
    std::string rebinding_shortcut_;  // 正在捕获按键的快捷键名，空 = 未捕获
    std::string rebind_conflict_;     // 上一次重绑定的冲突提示
};

const char* language_name(Language lang);

} // namespace gryce_engine::editor
