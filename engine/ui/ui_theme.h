#pragma once

#include <cstdint>

#include "export.h"

namespace gryce_engine::ui {

enum class UIThemeColor : uint8_t {
    Window = 0,
    Panel,
    PanelRaised,
    Input,
    Text,
    TextMuted,
    Border,
    Accent,
    AccentHover,
    AccentPressed,
    Selection,
    Success,
    Warning,
    Error,
    Overlay,
    Track,
    // Fluent 的控件填充：静止/悬停/按下三档，通常写成半透明白，叠在任何底色
    // 上都自动"提亮一档"，不必为每个背景配一套按钮色。
    ControlFill,
    ControlFillHover,
    ControlFillPressed,
    // 控件发丝边（ControlStrokeColor）：Fluent 里按钮/输入框静止态就有这一圈。
    ControlStroke,
    // 投影色（Fluent 的 elevation）：只在叠层/浮起控件上使用，alpha 决定浓度。
    Shadow,
    Count,
    Custom = 255,
};

enum class UIThemeMetric : uint8_t {
    Padding = 0,
    Gap,
    RowHeight,
    ControlHeight,
    BorderWidth,
    FontSize,
    SmallFontSize,
    // 分区标题字号（Fluent 的 Subtitle / Body Large）。标题、卡片标题用它，
    // 与控制字体的 14px 拉开层级；此前全界面只有 14/16 两档，读起来是平的。
    SubtitleFontSize,
    // 圆角阶梯：小半径给控件（按钮/输入框/复选框），中半径给卡片/面板，
    // 大半径给对话框/浮层。胶囊形（进度条、开关轨道）由控件按 h*0.5 自行取。
    RadiusSmall,
    RadiusMedium,
    RadiusLarge,
    // 静止态投影扩散半径（像素）。0 = 该主题不做 elevation。
    Elevation,
    // 交互动画时长（秒）。Fluent 的 fast/normal/slow 三档。
    MotionFast,
    MotionNormal,
    MotionSlow,
    Count,
};

enum class UIThemePreset : uint8_t {
    Dark = 0,
    Light,
    HighContrast,
    Nord,
    // WinUI/Fluent 的默认配色：中性灰表面 + 单一强调色。
    Fluent,
    Count,
};

// 控件声明"自己属于哪一档圆角"，具体像素值由主题给出。这样换主题的半径阶梯
// 时所有控件一起变，控件里不出现裸数字。
enum class UIRadiusTier : uint8_t {
    Small = 0,   // 按钮 / 输入框 / 复选框
    Medium,      // 卡片 / 面板
    Large,       // 对话框 / 浮层
    Count,
};

struct UITheme {
    const char* name = "Dark";
    uint32_t colors[static_cast<unsigned>(UIThemeColor::Count)] = {};
    float metrics[static_cast<unsigned>(UIThemeMetric::Count)] = {};
    uint32_t revision = 0;
};

GRYCE_API const UITheme& ui_theme();
GRYCE_API uint32_t ui_theme_revision();
GRYCE_API uint32_t ui_theme_color(UIThemeColor color);
GRYCE_API float ui_theme_metric(UIThemeMetric metric);
GRYCE_API float ui_theme_radius(UIRadiusTier tier);

GRYCE_API void set_ui_theme_preset(UIThemePreset preset);
GRYCE_API void set_ui_theme_color(UIThemeColor color, uint32_t rgba);
GRYCE_API void set_ui_theme_metric(UIThemeMetric metric, float value);
GRYCE_API int load_ui_theme_json(const char* json);
GRYCE_API int load_ui_theme_file(const char* path);

GRYCE_API uint32_t ui_theme_rgba(float r, float g, float b, float a = 1.0f);
GRYCE_API uint32_t ui_theme_mix(uint32_t from, uint32_t to, float amount);

} // namespace gryce_engine::ui
