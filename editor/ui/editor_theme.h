#pragma once

#include <string>

#include "engine_theme.h"
#include "engine_theme_modern.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorTheme — 兼容旧接口的薄封装
//
// 实际主题实现已迁移到 EngineTheme 命名空间。
// 这里保留 ThemePreset / ThemeConfig / apply_theme 等旧符号，
// 让 editor_app 和 settings_window 无需大面积改动。
// ---------------------------------------------------------------------------

enum class ThemePreset { Dark, Light, ModernLight };

struct ThemeConfig {
    // 保留字段，已不再参与主题应用；字体大小由 EngineTheme::LoadFonts 决定
    float font_size = 14.0f;
    // UI 全局缩放（1.0 = 原始，1.5 = 放大 50%）
    float ui_scale = EngineTheme::k_default_ui_scale;
};

ThemeConfig default_theme_config();

// 应用主题到当前 ImGui 上下文（含字体加载）
void apply_theme(ThemePreset preset, const ThemeConfig& config = {});

// 加载/重新加载字体。返回是否成功；失败时保持当前字体。
bool load_editor_font(const ThemeConfig& config = {});

// 将配置持久化/加载到 editor_theme.json（位于项目根目录）
void save_theme_config(const std::string& project_root, const ThemeConfig& config, ThemePreset preset);
bool load_theme_config(const std::string& project_root, ThemeConfig& out_config, ThemePreset& out_preset);

} // namespace gryce_engine::editor
