#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <imgui.h>

// ---------------------------------------------------------------------------
// EngineTheme — Unity 2023 + VS2026 风格的 ImGui 主题（Docking Branch）
//
// 命名空间：
//   - EngineTheme::Dark::Apply()
//   - EngineTheme::Light::Apply()
//   - EngineTheme::SelectableWithIndicator(...)
//   - EngineTheme::PropertyRow(...)
//   - EngineTheme::IconButton(...)
//   - EngineTheme::LoadFonts(...)
// ---------------------------------------------------------------------------

namespace EngineTheme {

constexpr ImU32 ColorFromHex(uint32_t rgb, uint8_t alpha = 0xFF) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

// ---------------------------------------------------------------------------
// Dark
// ---------------------------------------------------------------------------
namespace Dark {
constexpr ImU32 Bg0 = ColorFromHex(0x0D1117);
constexpr ImU32 Bg1 = ColorFromHex(0x161B22);
constexpr ImU32 Bg2 = ColorFromHex(0x21272E);
constexpr ImU32 Bg3 = ColorFromHex(0x30363D);
constexpr ImU32 Accent = ColorFromHex(0x2F81F7);
constexpr ImU32 TextPrimary = ColorFromHex(0xE6EDF3);
constexpr ImU32 TextSecondary = ColorFromHex(0x8B949E);
constexpr ImU32 Green = ColorFromHex(0x3FB950);
constexpr ImU32 Yellow = ColorFromHex(0xD29922);
constexpr ImU32 Red = ColorFromHex(0xF85149);

void Apply();
} // namespace Dark

// ---------------------------------------------------------------------------
// Light
// ---------------------------------------------------------------------------
namespace Light {
constexpr ImU32 Bg0 = ColorFromHex(0xFFFFFF);
constexpr ImU32 Bg1 = ColorFromHex(0xF6F8FA);
constexpr ImU32 Bg2 = ColorFromHex(0xEAEDF0);
constexpr ImU32 Bg3 = ColorFromHex(0xD0D7DE);
constexpr ImU32 Accent = ColorFromHex(0x096AD9);
constexpr ImU32 TextPrimary = ColorFromHex(0x1F2328);
constexpr ImU32 TextSecondary = ColorFromHex(0x656D76);
constexpr ImU32 Green = ColorFromHex(0x1A7F37);
constexpr ImU32 Yellow = ColorFromHex(0x9A6700);
constexpr ImU32 Red = ColorFromHex(0xCF222E);

void Apply();
} // namespace Light

// ---------------------------------------------------------------------------
// 共享辅助函数
// ---------------------------------------------------------------------------

// 左侧带 3px 色条的 Selectable（用于 Hierarchy / Project 等列表）
// indicator_color 默认使用当前主题强调色
bool SelectableWithIndicator(const char* label,
                             bool selected,
                             ImU32 indicator_color = Dark::Accent,
                             ImGuiSelectableFlags flags = 0,
                             const ImVec2& size_arg = ImVec2(0, 0));

// Inspector 风格的两列属性行：左侧 Label，右侧 Content（由 lambda 绘制）
void PropertyRow(const char* label,
                 const std::function<void()>& content,
                 float label_width = 120.0f);

// 28x28 透明图标按钮（icon_text 通常为字体图标字符）
bool IconButton(const char* id,
                const char* icon_text,
                const ImVec2& size = ImVec2(28.0f, 28.0f));

// 加载编辑器字体：
//   - UI 字体默认 Inter 14px（不存在时回退到 Roboto）
//   - 等宽字体默认 JetBrains Mono 13px（不存在时回退到 ImGui 默认等宽字体）
//   - 当前语言为中文时会合并系统 CJK 字体
// 返回是否成功加载 UI 字体
bool LoadFonts(const char* ui_font_path = nullptr,
               float ui_size = 14.0f,
               const char* mono_font_path = nullptr,
               float mono_size = 13.0f);

// 全局 UI 缩放因子（1.0 = 原始，1.5 = 放大 50%）
constexpr float k_default_ui_scale = 1.5f;

// 按比例缩放 ImGui 样式（padding、spacing、frame 大小等）
void ScaleStyle(float scale);

// 获取加载后的等宽字体（未加载或失败返回 nullptr）
ImFont* CodeFont();

} // namespace EngineTheme
