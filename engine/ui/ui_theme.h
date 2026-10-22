// ===========================================================================
// engine/ui/ui_theme.h — 主题 token（无 ImGui 依赖的纯数据 + 一次性落地）
//
// 解决的问题：控件颜色散落在各个调用点的 PushStyleColor 里，改一处主题要
// 翻遍所有面板，而"换一套浅色"这种需求直接没法做。这里收成一张 token 表：
// 颜色按语义命名（window_bg / accent / ok / warn ...），度量按用途命名
// （行高 / 内边距 / 缩进）。调用方只引用 token，不引用色值。
//
// 两层结构：
//   * token 本身是纯数据（Theme struct），任何时候可读可写，与 ImGui 无关；
//   * apply() 把 token 一次性推成 ImGui 的 Style——之后新画的控件自动跟上。
//     没有 ImGui 的构建里 apply() 是 no-op，token 照样可读（无窗口宿主
//     也能跑同一份界面逻辑）。
//
// 语义文本色（GUI_TEXT_DIM / OK / WARN / ERROR / ACCENT）也从这里取：
// GUi_PushTextStyle 与 GUi_TextColored 不再有硬编码色值。
//
// 线程模型：token 读写在 UI 线程；apply() 由宿主在帧外调用（改主题是低频
// 操作，不值得为此加锁）。
// ===========================================================================

#ifndef GRYCE_UI_THEME_H
#define GRYCE_UI_THEME_H

#include <cstdint>

namespace gryce_ui {

// 颜色打包格式：0xRRGGBBAA。
enum ThemePreset : int {
    THEME_PRESET_DARK  = 0,
    THEME_PRESET_LIGHT = 1,
};

enum ThemeColor : int {
    THEME_COLOR_WINDOW_BG = 0,
    THEME_COLOR_CHILD_BG,
    THEME_COLOR_BORDER,
    THEME_COLOR_TEXT,
    THEME_COLOR_TEXT_DIM,
    THEME_COLOR_ACCENT,
    THEME_COLOR_ACCENT_HOVER,
    THEME_COLOR_ACCENT_ACTIVE,
    THEME_COLOR_BUTTON,
    THEME_COLOR_BUTTON_HOVER,
    THEME_COLOR_BUTTON_ACTIVE,
    THEME_COLOR_HEADER,
    THEME_COLOR_HEADER_HOVER,
    THEME_COLOR_SELECTION,
    THEME_COLOR_OK,
    THEME_COLOR_WARN,
    THEME_COLOR_ERROR,
    THEME_COLOR_COUNT,
};

enum ThemeMetric : int {
    THEME_METRIC_PAD_X = 0,     // 控件水平内边距
    THEME_METRIC_PAD_Y,         // 控件垂直内边距
    THEME_METRIC_ROW_H,         // 标准行高（布局栈默认行高）
    THEME_METRIC_ITEM_SPACING,  // 行 / 单元格间距
    THEME_METRIC_INDENT,        // 树形每级缩进
    THEME_METRIC_BORDER_W,      // 边框宽度
    THEME_METRIC_COUNT,
};

struct Theme {
    uint32_t colors[THEME_COLOR_COUNT];
    float    metrics[THEME_METRIC_COUNT];
};

// 当前主题（进程内唯一，默认深色）。返回引用：调用方读取后立即用，
// 不要长期缓存（主题随时可能被换）。
const Theme& theme();

// 整表切换预设（深色 / 浅色）。之后再 set_color 调整个别 token。
void set_preset(int preset);

// 读写单个 token。token 越界时忽略（写）/ 返回 0（读）。
void  set_color(int token, uint32_t rgba);
uint32_t color(int token);
void  set_metric(int token, float value);
float metric(int token);

// 把当前 token 落地到 ImGui Style。帧外调用；每次换主题后调一次即可。
// 没有 ImGui 的构建里是 no-op。
void apply();

} // namespace gryce_ui

#endif // GRYCE_UI_THEME_H
