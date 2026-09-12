#pragma once

#include <string>

#include "editor_theme.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorSettings — 编辑器偏好持久化单例。
// 以 JSON 存储到 %APPDATA%/GryceEngine/editor_settings.json（非 Windows 回落
// 到可执行文件目录），覆盖窗口尺寸/最大化、主题、界面语言（语言为后续 i18n 预留）。
// ---------------------------------------------------------------------------
class EditorSettings {
public:
    static EditorSettings& instance();

    void load();
    void save();

    int  window_width() const { return window_width_; }
    void set_window_width(int v);
    int  window_height() const { return window_height_; }
    void set_window_height(int v);
    bool window_maximized() const { return window_maximized_; }
    void set_window_maximized(bool v);

    EditorTheme theme() const { return theme_; }
    void set_theme(EditorTheme v);

    // 界面语言（预留，未实现 i18n 切换）。
    const std::string& language() const { return language_; }
    void set_language(const std::string& v);

    std::string config_path() const;

private:
    EditorSettings() = default;

    int  window_width_ = 1600;
    int  window_height_ = 900;
    bool window_maximized_ = false;
    EditorTheme theme_ = EditorTheme::Dark;
    std::string language_ = "zh";
};

} // namespace gryce_engine::editor