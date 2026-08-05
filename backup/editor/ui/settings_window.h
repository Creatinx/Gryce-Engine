#pragma once

#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "editor_theme.h"
#include "../localization/localization.h"
#include "../shortcuts/shortcut_manager.h"

namespace gryce_engine::render { class RenderContext; }

namespace gryce_engine::editor {

class ShortcutManager;

// ---------------------------------------------------------------------------
// SettingsWindow — 编辑器设置窗口（File > Settings）
//
// JetBrains Settings 风格布局：
//   - 左侧 ~240px 栏目树（分组可展开/折叠，顶部搜索框过滤页面）
//   - 右侧内容区：顶部面包屑（分组 › 页面）+ 当前页设置项
//   - 底部右侧：确定 / 取消 / 应用
//
// 编辑是暂存的：打开时快照一份设置副本，所有控件修改副本；
// 确定/应用 提交（应用生效 + 持久化），取消/X 丢弃并还原快捷键快照。
// ---------------------------------------------------------------------------

struct ApplianceSettings {
    Language language = Language::English;
};

// 编辑器行为设置（持久化到 editor_settings.json 的 "editor" 组）
struct EditorBehaviorSettings {
    bool vsync = true;                 // 垂直同步
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

    // 注入渲染上下文：VSync 提交时调用 set_swap_interval。
    void set_render_context(render::RenderContext* ctx) { render_ctx_ = ctx; }

    using RebuildFontsFn = std::function<void()>;
    void set_rebuild_fonts_callback(RebuildFontsFn fn) { rebuild_fonts_fn_ = std::move(fn); }

    // 注入快捷键管理器：快捷键页的展示与重绑定。
    void set_shortcut_manager(ShortcutManager* mgr) { shortcut_mgr_ = mgr; }

    void open() { open_ = true; just_opened_ = true; }
    bool is_open() const { return open_; }

    // 设置页面（栏目树叶子节点）
    enum class Page { Appearance, Language, EditorGeneral, Shortcuts };

    struct PageInfo {
        Page page;
        const char* name_key;   // 页面名本地化 key
        const char* group_key;  // 所属分组本地化 key（空 = 顶层叶子）
    };

private:
    void draw_sidebar_tree();
    void draw_breadcrumb(const PageInfo& info);
    void draw_footer_buttons(EditorSettings& settings);
    void draw_appearance_page();
    void draw_language_page();
    void draw_editor_general_page();
    void draw_shortcuts_page();

    // 提交（确定/应用）：暂存副本 → 正式设置，应用生效并持久化
    void commit(EditorSettings& settings);
    // 取消/X：丢弃暂存副本并还原快捷键快照
    void cancel_and_close();

    bool page_matches_filter(const PageInfo& info) const;
    bool group_matches_filter(const char* group_key) const;

    bool open_ = false;
    bool just_opened_ = false;
    Page current_page_ = Page::Appearance;

    EditorSettings staged_;            // 暂存的设置副本
    bool dirty_ = false;               // 副本相对正式设置有改动
    char search_buf_[64] = {};

    // 打开窗口时的快捷键快照（取消时还原）
    std::vector<std::pair<std::string, ShortcutManager::KeyCombo>> shortcut_snapshot_;

    std::string project_root_;
    render::RenderContext* render_ctx_ = nullptr;
    ShortcutManager* shortcut_mgr_ = nullptr;
    RebuildFontsFn rebuild_fonts_fn_;
    std::string rebinding_shortcut_;  // 正在捕获按键的快捷键名，空 = 未捕获
    std::string rebind_conflict_;     // 上一次重绑定的冲突提示
};

const char* language_name(Language lang);

} // namespace gryce_engine::editor
