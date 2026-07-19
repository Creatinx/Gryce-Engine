#pragma once

#include <string>

#include <imgui.h>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorTheme — Fluent Design 风格的 ImGui 主题（深色 / 浅色）
// ---------------------------------------------------------------------------
// 提供强调色 Hue 自定义、圆角、字体加载与浅色/深色切换。
// 颜色按 Fluent Design 规范选取：背景层 / 表面层 / 卡片层 + 强调色梯度。
// ---------------------------------------------------------------------------

enum class ThemePreset { Dark, Light };

struct ThemeConfig {
    // 强调色色相 [0,1]，默认 Fluent 蓝 (~206°/360°)
    float accent_hue = 206.0f / 360.0f;
    // 全局字体大小
    float font_size = 16.0f;
    // 自定义字体路径；空字符串时使用项目内置 Roboto
    std::string font_path;
    // 全局圆角半径
    float rounding = 6.0f;
    // 是否启用轻微阴影/深度效果（通过 Border/Alpha 模拟）
    bool shadow = true;
};

ThemeConfig default_theme_config();

// 应用主题到当前 ImGui 上下文。首次调用会加载字体；之后切换主题时字体若已加载则复用。
void apply_theme(ThemePreset preset, const ThemeConfig& config = {});

// 加载/重新加载字体。返回是否成功；失败时保持当前字体。
bool load_editor_font(const ThemeConfig& config);

// 将配置持久化/加载到 editor_theme.json（位于项目根目录）
void save_theme_config(const std::string& project_root, const ThemeConfig& config, ThemePreset preset);
bool load_theme_config(const std::string& project_root, ThemeConfig& out_config, ThemePreset& out_preset);

} // namespace gryce_engine::editor
