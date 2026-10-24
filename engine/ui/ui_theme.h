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
// 预设：每套预设只写 17 个"锚色"（窗口/文本/强调/按钮/标题/选择/语义色），
// 其余 40 个颜色 token 由锚色按固定规则派生（hover = 提亮、active = 再提亮、
// separator = border ...）。这样新增一套主题只需给 17 个值，且派生规则本身
// 可单测（"换预设后 CHECK_MARK 恒等于 ACCENT"这类一致性不靠肉眼）。
//
// 追加新 token 只能加在枚举末尾：编号被 C 头（ui_api.h）镜像并用静态断言
// 钉死，插到中间会让两边的编号整体漂移。
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
    THEME_PRESET_MIDNIGHT = 2,     // 深蓝夜航
    THEME_PRESET_NORD  = 3,        // 北极青灰
    THEME_PRESET_HIGH_CONTRAST = 4,// 纯黑高对比
    THEME_PRESET_COUNT = 5,
};

// 预设显示名（越界返回 ""，不返回空指针）。
const char* theme_preset_name(int preset);

// 锚色：每套预设必须给出的 17 个值（顺序即枚举顺序，见 ui_theme.cpp）。
enum ThemeAnchor : int {
    THEME_ANCHOR_WINDOW_BG = 0,
    THEME_ANCHOR_CHILD_BG,
    THEME_ANCHOR_BORDER,
    THEME_ANCHOR_TEXT,
    THEME_ANCHOR_TEXT_DIM,
    THEME_ANCHOR_ACCENT,
    THEME_ANCHOR_ACCENT_HOVER,
    THEME_ANCHOR_ACCENT_ACTIVE,
    THEME_ANCHOR_BUTTON,
    THEME_ANCHOR_BUTTON_HOVER,
    THEME_ANCHOR_BUTTON_ACTIVE,
    THEME_ANCHOR_HEADER,
    THEME_ANCHOR_HEADER_HOVER,
    THEME_ANCHOR_SELECTION,
    THEME_ANCHOR_OK,
    THEME_ANCHOR_WARN,
    THEME_ANCHOR_ERROR,
    THEME_ANCHOR_COUNT,
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
    // ---- 以下为美化扩充（由锚色派生，预设不直接给值） ----
    THEME_COLOR_POPUP_BG,
    THEME_COLOR_TITLE_BG,
    THEME_COLOR_TITLE_BG_ACTIVE,
    THEME_COLOR_TITLE_BG_COLLAPSED,
    THEME_COLOR_MENUBAR_BG,
    THEME_COLOR_SCROLLBAR_BG,
    THEME_COLOR_SCROLLBAR_GRAB,
    THEME_COLOR_SCROLLBAR_GRAB_HOVERED,
    THEME_COLOR_SCROLLBAR_GRAB_ACTIVE,
    THEME_COLOR_SEPARATOR,
    THEME_COLOR_SEPARATOR_HOVERED,
    THEME_COLOR_SEPARATOR_ACTIVE,
    THEME_COLOR_RESIZE_GRIP,
    THEME_COLOR_RESIZE_GRIP_HOVERED,
    THEME_COLOR_RESIZE_GRIP_ACTIVE,
    THEME_COLOR_CHECK_MARK,
    THEME_COLOR_SLIDER_GRAB,
    THEME_COLOR_SLIDER_GRAB_ACTIVE,
    THEME_COLOR_HEADER_ACTIVE,
    THEME_COLOR_PLOT_LINES,
    THEME_COLOR_PLOT_LINES_HOVERED,
    THEME_COLOR_PLOT_HISTOGRAM,
    THEME_COLOR_PLOT_HISTOGRAM_HOVERED,
    THEME_COLOR_TABLE_HEADER_BG,
    THEME_COLOR_TABLE_BORDER_STRONG,
    THEME_COLOR_TABLE_BORDER_LIGHT,
    THEME_COLOR_TABLE_ROW_BG,
    THEME_COLOR_TABLE_ROW_BG_ALT,
    THEME_COLOR_DRAG_DROP_TARGET,
    THEME_COLOR_NAV_HIGHLIGHT,
    THEME_COLOR_NAV_WINDOWING_HIGHLIGHT,
    THEME_COLOR_NAV_WINDOWING_DIM_BG,
    THEME_COLOR_MODAL_WINDOW_DIM_BG,
    THEME_COLOR_BORDER_SHADOW,
    THEME_COLOR_TAB,
    THEME_COLOR_TAB_HOVERED,
    THEME_COLOR_TAB_SELECTED,
    THEME_COLOR_TAB_SELECTED_OVERLINE,
    THEME_COLOR_TAB_DIMMED,
    THEME_COLOR_TAB_DIMMED_SELECTED,
    // 控件（按钮/输入框/滑杆槽）的日常描边：比 BORDER 更贴近控件自身底色，
    // 只用来"勾出形状"而不是"画一圈亮线"。悬停/聚焦仍换 ACCENT。
    THEME_COLOR_CONTROL_BORDER,
    THEME_COLOR_COUNT,
};

enum ThemeMetric : int {
    THEME_METRIC_PAD_X = 0,     // 控件水平内边距
    THEME_METRIC_PAD_Y,         // 控件垂直内边距
    THEME_METRIC_ROW_H,         // 标准行高（布局栈默认行高）
    THEME_METRIC_ITEM_SPACING,  // 行 / 单元格间距
    THEME_METRIC_INDENT,        // 树形每级缩进
    THEME_METRIC_BORDER_W,      // 边框宽度
    // ---- 以下为美化扩充（圆角/滚动条，ImGui 默认值偏方正） ----
    THEME_METRIC_WINDOW_ROUNDING,
    THEME_METRIC_CHILD_ROUNDING,
    THEME_METRIC_FRAME_ROUNDING,
    THEME_METRIC_GRAB_ROUNDING,
    THEME_METRIC_POPUP_ROUNDING,
    THEME_METRIC_TAB_ROUNDING,
    THEME_METRIC_SCROLLBAR_SIZE,
    THEME_METRIC_GRAB_MIN_SIZE,
    THEME_METRIC_COUNT,
};

struct Theme {
    uint32_t colors[THEME_COLOR_COUNT];
    float    metrics[THEME_METRIC_COUNT];
};

// ---------------------------------------------------------------------------
// ThemeAnim — 主题自带的交互动画参数（JSON 主题 "animation" 块）
//
// 驱动内置轨的"状态过渡"：悬停/按下时控件主填充色在 from/to 之间按
// ease 插值。enabled=false（或时长为 0）时一切瞬变——单测与截图用
// time=0 的输入天然走这条路径，画面可复现。
// ---------------------------------------------------------------------------
struct ThemeAnim {
    bool     enabled  = false;
    float    hover_s  = 0.12f;   // 悬停/普通状态过渡时长（秒）
    float    press_s  = 0.08f;   // 按下状态过渡时长（秒，更跟手）
    int      ease     = 5;       // AnimEase（ui_anim.h；默认 EaseOutCubic）
};

// 当前主题的动画参数（与 theme() 同为进程内唯一、UI 线程、无锁）。
const ThemeAnim& theme_anim();
void set_theme_anim(const ThemeAnim& a);

// ---------------------------------------------------------------------------
// JSON 主题加载
//
// 文件/文本格式（全部可选，缺什么补什么）：
//   {
//     "name": "Fluent Dark",
//     "anchors": { "window_bg": "#202020", "accent": "#4CC2FFFF", ... },
//     "colors":  { "control_border": "#333333", ... },   // 派生后覆盖
//     "metrics": { "window_rounding": 8, ... },
//     "animation": { "enabled": true, "hover_ms": 100, "press_ms": 70,
//                    "ease": "out_cubic" }
//   }
// 颜色写 "#RRGGBB" / "#RRGGBBAA"（# 可省略）。语义：以深色预设的锚色为
// 底 → 套 anchors → derive_extended → 套 colors 覆盖 → 套 metrics/animation。
// 部分主题文件（只写 anchors）因此是良定义的。返回 1/0（0 = JSON 非法）。
// ---------------------------------------------------------------------------
int load_theme_json(const char* json_text);
int load_theme_file(const char* path);

// 当前主题（进程内唯一，默认深色）。返回引用：调用方读取后立即用，
// 不要长期缓存（主题随时可能被换）。
const Theme& theme();

// 整表切换预设（越界按深色）。之后再 set_color 调整个别 token。
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
