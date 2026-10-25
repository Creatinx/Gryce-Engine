// engine/ui/ui_widget.cpp
// retained 控件层实现。语义见 ui_widget.h 头部。

#include "ui/ui_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/ui_anim.h"

namespace gryce_ui {

// ---------------------------------------------------------------------------
// 文本度量（默认估算）
// ---------------------------------------------------------------------------

float ui_approx_text_width(const char* text, float font_size) {
    if (!text || !*text || font_size <= 0.0f) return 0.0f;
    float units = 0.0f;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        // 解一个 UTF-8 码点：ASCII 半宽、其余全宽。解不出来（坏字节）按
        // 替换字符算，不崩、不无限循环。
        uint32_t cp  = 0xFFFDu;
        int      len = 1;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0u) == 0xC0u) {
            cp = *p & 0x1Fu; len = 2;
        } else if ((*p & 0xF0u) == 0xE0u) {
            cp = *p & 0x0Fu; len = 3;
        } else if ((*p & 0xF8u) == 0xF0u) {
            cp = *p & 0x07u; len = 4;
        }
        for (int i = 1; i < len && p[i]; ++i) cp = (cp << 6) | (p[i] & 0x3Fu);
        p += len;
        units += (cp < 0x80u) ? 0.5f : 1.0f;
    }
    return units * font_size;
}

float UiTextMetrics::width(const char* text, float font_size) const {
    return ui_approx_text_width(text, font_size);
}

float UiTextMetrics::line_height(float font_size) const {
    return font_size * 1.25f;
}

// ---------------------------------------------------------------------------
// 剪贴板
// ---------------------------------------------------------------------------

UiClipboard& ui_clipboard() {
    static UiClipboard clipboard_;
    return clipboard_;
}

// ---------------------------------------------------------------------------
// UTF-8 工具
// ---------------------------------------------------------------------------

namespace {

// 解一个码点，返回占用字节数（>=1）。坏字节按单字节跳过。
int utf8_seq_len(const char* s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    int n = 1;
    if      ((c & 0xE0u) == 0xC0u) n = 2;
    else if ((c & 0xF0u) == 0xE0u) n = 3;
    else if ((c & 0xF8u) == 0xF0u) n = 4;
    for (int i = 1; i < n; ++i) {
        if (!s[i]) return i;   // 截断的多字节序列：只走已有部分
    }
    return n;
}

inline bool rect_contains(const Rect& r, float x, float y) {
    return x >= r.x && x <= r.right() && y >= r.y && y <= r.bottom();
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int clampint(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 把一个 Unicode 码点按 UTF-8 追加进 buf（保证 '\0' 结尾；放不下整码点则不加）。
void utf8_append(char* buf, size_t cap, uint32_t cp) {
    const size_t len = std::strlen(buf);
    char tmp[4] = {};
    int  n = 0;
    if (cp < 0x80u) {
        tmp[0] = static_cast<char>(cp);
        n = 1;
    } else if (cp < 0x800u) {
        tmp[0] = static_cast<char>(0xC0u | (cp >> 6));
        tmp[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        tmp[0] = static_cast<char>(0xE0u | (cp >> 12));
        tmp[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        tmp[0] = static_cast<char>(0xF0u | (cp >> 18));
        tmp[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    if (len + static_cast<size_t>(n) + 1 > cap) return;
    for (int i = 0; i < n; ++i) buf[len + i] = tmp[i];
    buf[len + n] = '\0';
}

// 删掉最后一个 UTF-8 码点（回退过所有续字节 10xxxxxx）。
void utf8_pop(char* buf) {
    const size_t len = std::strlen(buf);
    if (len == 0) return;
    size_t i = len - 1;
    while (i > 0 && (static_cast<unsigned char>(buf[i]) & 0xC0u) == 0x80u) --i;
    buf[i] = '\0';
}

// 角半径钳制：超过半边长度的圆角画出来是"药丸"，控件高度只有 20~24 像素，
// 不钳就会把按钮压成胶囊（视觉上比圆角更抢眼）。
float clamp_radius(float radius, const Rect& r) {
    float m = (r.w < r.h) ? r.w : r.h;
    m *= 0.5f;
    if (radius > m) radius = m;
    return (radius > 0.0f) ? radius : 0.0f;
}

// 颜色微调：f > 0 提亮、f < 0 压暗，alpha 不变。面板/标题栏的竖直渐变靠它
// 生成"上亮下暗"的两端色——不用为每套主题多加一个 token，换预设自动跟着变。
uint32_t shade(uint32_t rgba, float f) {
    auto adjust = [f](uint32_t v) {
        const float t = static_cast<float>(v) / 255.0f;
        const float out = (f >= 0.0f) ? (t + (1.0f - t) * f) : (t * (1.0f + f));
        const float c = (out < 0.0f) ? 0.0f : (out > 1.0f ? 1.0f : out);
        return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return (adjust((rgba >> 24) & 0xFFu) << 24) |
           (adjust((rgba >> 16) & 0xFFu) << 16) |
           (adjust((rgba >> 8) & 0xFFu) << 8) |
           (rgba & 0xFFu);
}

// 两色按 t 混合（含 alpha）：控件描边、软高亮都靠它从两个 token 之间取值，
// 不必为"半亮不亮"的中间态各加一个 token。
uint32_t mix_rgba(uint32_t a, uint32_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    auto pick = [a, b, t](int shift) {
        const float va = static_cast<float>((a >> shift) & 0xFFu);
        const float vb = static_cast<float>((b >> shift) & 0xFFu);
        return static_cast<uint32_t>(va + (vb - va) * t + 0.5f);
    };
    return (pick(24) << 24) | (pick(16) << 16) | (pick(8) << 8) | pick(0);
}

// 相对亮度（sRGB → 线性，Rec.709 权重）。判断"这个颜色上该压白字还是黑字"
// 只能用线性亮度——直接把 0x4CC2FF 的 RGB 平均值当亮度会得出"中等灰"，
// 于是在蓝底上压深灰字，看着像脏了一块。
float rel_luma(uint32_t rgba) {
    auto lin = [](uint32_t v) {
        const float s = static_cast<float>(v) / 255.0f;
        return (s <= 0.04045f) ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin((rgba >> 24) & 0xFFu) + 0.7152f * lin((rgba >> 16) & 0xFFu) +
           0.0722f * lin((rgba >> 8) & 0xFFu);
}

// 圆角填充 + 描边（描边向内生长，外轮廓尺寸不因加边框变大）
void draw_round_frame(UiDrawList& dl, const Rect& r, float radius, uint32_t fill,
                      uint32_t border, float bw) {    if (r.w <= 0.0f || r.h <= 0.0f) return;
    const float rad = clamp_radius(radius, r);
    if ((fill & 0xFFu) != 0u) dl.add_round_rect(r.x, r.y, r.w, r.h, rad, fill);
    if (bw > 0.0f && (border & 0xFFu) != 0u) {
        dl.add_round_border(r.x, r.y, r.w, r.h, rad, bw, border);
    }
}

// 圆角渐变填充 + 描边（按钮/标题栏这类"有厚度感"的面板）
void draw_round_frame_gradient(UiDrawList& dl, const Rect& r, float radius,
                               uint32_t top, uint32_t bottom,
                               uint32_t border, float bw) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    const float rad = clamp_radius(radius, r);
    if ((top & 0xFFu) != 0u) {
        dl.add_round_gradient(r.x, r.y, r.w, r.h, rad, top, bottom);
    }
    if (bw > 0.0f && (border & 0xFFu) != 0u) {
        dl.add_round_border(r.x, r.y, r.w, r.h, rad, bw, border);
    }
}

uint32_t theme_color(const Theme& t, int token) {
    return (token >= 0 && token < THEME_COLOR_COUNT) ? t.colors[token] : 0xFFFFFFFFu;
}

float theme_metric(const Theme& t, int token, float fallback) {
    return (token >= 0 && token < THEME_METRIC_COUNT) ? t.metrics[token] : fallback;
}

// 压在这个填充色上面的字/点该用什么色：在 TEXT 与 WINDOW_BG 里挑与 fill 亮度
// 相差更远的那个。固定用 TEXT 的话，浅色主题下"蓝底 + 近黑字"就糊成一团。
uint32_t on_fill(uint32_t fill, const Theme& t) {
    const float f = rel_luma(fill);
    const uint32_t a = theme_color(t, THEME_COLOR_TEXT);
    const uint32_t b = theme_color(t, THEME_COLOR_WINDOW_BG);
    const float da = rel_luma(a) - f;
    const float db = rel_luma(b) - f;
    return ((da < 0.0f ? -da : da) >= (db < 0.0f ? -db : db)) ? a : b;
}

// ---- 状态过渡动画（主题 animation 块驱动） ----
//
// 参与动画的控件种类与其"主填充色"目标。绘制与推进共用同一份目标，
// 保证动画落点与从未开过动画的静态帧逐字节一致（单测走的就是静态路径）。

bool widget_anim_fill(const Widget& w, const Theme& t, uint32_t* out) {
    switch (w.kind) {
        case WidgetKind::Button:
            *out = !w.enabled ? theme_color(t, THEME_COLOR_BUTTON)
                 : w.pressed  ? theme_color(t, THEME_COLOR_BUTTON_ACTIVE)
                 : w.hovered  ? theme_color(t, THEME_COLOR_BUTTON_HOVER)
                              : theme_color(t, THEME_COLOR_BUTTON);
            return true;
        case WidgetKind::DragFloat:
            *out = w.pressed ? theme_color(t, THEME_COLOR_BUTTON_ACTIVE)
                 : w.hovered ? theme_color(t, THEME_COLOR_BUTTON_HOVER)
                             : theme_color(t, THEME_COLOR_BUTTON);
            return true;
        case WidgetKind::Checkbox:
            *out = w.checked ? theme_color(t, THEME_COLOR_ACCENT)
                 : w.hovered ? theme_color(t, THEME_COLOR_BUTTON_HOVER)
                             : theme_color(t, THEME_COLOR_BUTTON);
            return true;
        case WidgetKind::Slider: {
            // 滑块是"框即轨道"（ImGui 的 SliderScalar 用 FrameBg/FrameBgHovered/
            // FrameBgActive 画整条框），底色跟输入框完全一样，不是强调色。
            // 注意别再用 on_fill(ACCENT)：那是老版"强调色滑条"的取值，本主题
            // 的 ACCENT 是深色底 + 白字，on_fill 会算出纯白，整条框糊成白板。
            *out = w.pressed ? theme_color(t, THEME_COLOR_BUTTON_ACTIVE)
                 : w.hovered ? theme_color(t, THEME_COLOR_BUTTON_HOVER)
                             : theme_color(t, THEME_COLOR_BUTTON);
            return true;
        }
        case WidgetKind::MenuItem: {
            // 高亮块：悬停淡入、移开淡出（目标 = 同色 alpha 清 0）
            uint32_t c = mix_rgba(theme_color(t, THEME_COLOR_NAV_HIGHLIGHT),
                                  theme_color(t, THEME_COLOR_POPUP_BG), 0.65f);
            if (!(w.hovered && w.enabled)) c &= 0xFFFFFF00u;
            *out = c;
            return true;
        }
        case WidgetKind::Header:
            *out = w.hovered ? theme_color(t, THEME_COLOR_HEADER_HOVER)
                             : theme_color(t, THEME_COLOR_HEADER);
            return true;
        case WidgetKind::Selectable: {
            // 列表项的底色：选中恒亮、悬停淡入、其余透明（不画底）。
            // 与 ImGui 的 Selectable 一致：选中项即使鼠标不在上面也保持高亮。
            uint32_t c = theme_color(t, THEME_COLOR_HEADER);
            if (w.hovered) c = theme_color(t, THEME_COLOR_HEADER_HOVER);
            if (w.pressed && w.hovered) c = theme_color(t, THEME_COLOR_HEADER_ACTIVE);
            if (!w.selected && !w.hovered) c &= 0xFFFFFF00u;
            *out = c;
            return true;
        }
        case WidgetKind::Image:
            // 图片没有底色：画了会把视口/贴图预览盖住。返回 false 让调用方
            // 连淡入淡出都不做（Image 也没有 anim 目标色）。
            return false;
        default:
            return false;
    }
}

// 动画集合控件的"当前主填充色"。anim_to == 0（推进从未跑过）或动画关闭时
// 直接取静态目标；时钟走满后精确落在目标色（缓动形状不影响端点）。
uint32_t widget_fill_now(const Widget& w, const Theme& t) {
    const ThemeAnim& a = theme_anim();
    if (w.anim_to == 0 || !a.enabled) {
        uint32_t target = theme_color(t, THEME_COLOR_BUTTON);
        widget_anim_fill(w, t, &target);
        return target;
    }
    const float dur = w.pressed ? a.press_s : a.hover_s;
    if (dur <= 0.0f) return w.anim_to;
    const float k = anim_ease(static_cast<AnimEase>(a.ease), w.anim_clock / dur);
    return mix_rgba(w.anim_from, w.anim_to, k);
}

// 折叠标记：小方框里一个 + / -（drawlist 只有矩形与文本，画不了三角形；
// 用 ASCII 字符是唯一能保证任意字体都出字的做法）。
void draw_fold_marker(UiDrawList& dl, float x, float y, bool open,
                      const Theme& theme, const UiTextMetrics& metrics, float fs) {
    const uint32_t c = theme_color(theme, THEME_COLOR_TEXT_DIM);
    const float side = fs * 0.75f;
    const Rect box{x, y + (fs * 1.25f - side) * 0.5f, side, side};
    dl.add_round_border(box.x, box.y, box.w, box.h, 2.0f, 1.0f, c);
    const char* mark = open ? "-" : "+";
    const float tw = metrics.width(mark, fs);
    dl.add_text(box.x + (side - tw) * 0.5f, y, mark, fs, c);
}

// ---------------------------------------------------------------------------
// ImGui 对齐原语
//
// 内置轨的目标不是"另一套好看的设计"，而是**与 ImGui 轨逐像素一致**：同一份
// 主题 token、同一套几何规则。下面几个小函数就是那条对齐线上的公共因子——
// 全部按 ImGui 的实现直接换算（注释里给了对应的 ImGui 源函数名），几何常数
// 一个都不要就地魔改：改一处就会让两轨在那一处开始漂。
// ---------------------------------------------------------------------------

// ImGuiStyle::ItemInnerSpacing.x 的默认值：控件框与它右侧标签的间隙。
constexpr float kItemInnerSpacing = 4.0f;

// 控件框：填充 + 1px 边框。对应 ImGui 的 RenderFrame() + RenderFrameBorder()：
// 先填后描、描边向内吃一圈，所以框的外轮廓尺寸不会被边框撑大。
void draw_frame_box(UiDrawList& dl, const Rect& r, float radius, uint32_t fill,
                    uint32_t border, float bw) {
    dl.add_round_rect(r.x, r.y, r.w, r.h, radius, fill);
    if (bw > 0.0f) dl.add_round_border(r.x, r.y, r.w, r.h, radius, bw, border);
}

// 框内居中的单行文字（ImGui 的 RenderTextClipped(..., align=(0.5,0.5))）。
void draw_text_centered(UiDrawList& dl, const Rect& r, const char* s, float fs,
                        const UiTextMetrics& metrics, uint32_t col) {
    if (!s || !s[0]) return;
    const float tw = metrics.width(s, fs);
    dl.add_text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - fs) * 0.5f, s, fs, col);
}

// 控件右侧的标签（ImGui 的 RenderText(bb.Max.x + ItemInnerSpacing.x, bb.Min.y + FramePadding.y)）。
// 22 高的框 + 14 的字号下，FramePadding.y(4) 与垂直居中是同一个位置，
// 所以这里用居中算，框高变了也不会掉出去。
void draw_label_right(UiDrawList& dl, const Widget& w, const UiTextMetrics& metrics,
                      float fs, uint32_t col) {
    if (!w.label[0]) return;
    dl.add_text(w.rect.right() + kItemInnerSpacing,
                w.rect.y + (w.rect.h - fs) * 0.5f, w.label, fs, col);
}

// 滑块/数值框的显示文本。fmt 是 ImGui 风格的 printf 单值格式（"%.2f"）；
// 空串按 "%.3f"——与 ImGui 的默认值一致。fmt 里若写了两个及以上 % 会被
// 当成危险格式，直接退回默认（printf 的参数只有一个，多写会读到栈上的垃圾）。
void format_widget_value(char* out, int cap, const char* fmt, float value) {
    if (!out || cap <= 1) return;
    int conversions = 0;
    for (const char* p = fmt; p && *p; ++p) {
        if (*p == '%') {
            if (p[1] == '%') { ++p; continue; }
            ++conversions;
        }
    }
    const char* use = (fmt && fmt[0] && conversions == 1) ? fmt : "%.3f";
    std::snprintf(out, static_cast<size_t>(cap), use, static_cast<double>(value));
}

// 勾选标记：复刻 ImGui::RenderCheckMark 的三点折线（线宽 = sz/5）。
// ImGui 用 PathStroke 描边，drawlist 只有凸多边形填充，所以每段折线画成一个
// 四边形、拐点补一个同宽的方块把接缝盖掉——16px 的框里两者是同一个像素结果。
void draw_check_mark(UiDrawList& dl, const Rect& box, uint32_t col) {
    const float pad = (box.w / 6.0f >= 1.0f) ? std::floor(box.w / 6.0f) : 1.0f;
    float sz = box.w - pad * 2.0f;
    float thickness = sz / 5.0f;
    if (thickness < 1.0f) thickness = 1.0f;
    sz -= thickness * 0.5f;
    const float px = box.x + pad + thickness * 0.25f;
    const float py = box.y + pad + thickness * 0.25f;

    const float third = sz / 3.0f;
    const float bx = px + third;
    const float by = py + sz - third * 0.5f;
    const float p0x = bx - third,     p0y = by - third;
    const float p1x = bx,             p1y = by;
    const float p2x = bx + third * 2.0f, p2y = by - third * 2.0f;

    const float half = thickness * 0.5f;
    // 每段折线 = 一个平行四边形（法线方向偏移 ±half）
    auto stroke = [&](float x0, float y0, float x1, float y1) {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        const float nx = -dy / len * half;
        const float ny = dx / len * half;
        const float quad[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny,
                               x1 - nx, y1 - ny, x0 - nx, y0 - ny};
        dl.add_polygon(quad, 4, col);
    };
    stroke(p0x, p0y, p1x, p1y);
    stroke(p1x, p1y, p2x, p2y);
    const float joint[8] = {p1x - half, p1y - half, p1x + half, p1y - half,
                            p1x + half, p1y + half, p1x - half, p1y + half};
    dl.add_polygon(joint, 4, col);
}

// 下拉箭头：复刻 ImGui::RenderArrow(ImGuiDir_Down, scale=1)。
// (x, y) 是箭头方框左上角，方框边长 = 字号，三角形以方框中心为中心。
void draw_arrow_down(UiDrawList& dl, float x, float y, float fs, uint32_t col) {
    const float r = fs * 0.40f;
    const float cx = x + fs * 0.5f;
    const float cy = y + fs * 0.5f;
    const float tri[6] = {cx,                cy + 0.750f * r,
                          cx - 0.866f * r,   cy - 0.750f * r,
                          cx + 0.866f * r,   cy - 0.750f * r};
    dl.add_polygon(tri, 3, col);
}

// 滑块抓柄：复刻 ImGui 的 SliderBehavior 几何——左右各留 2px，抓柄宽度取
// GrabMinSize（主题 token），位置按比例在"可滑动区间"里插值，纵向内缩 2px。
// 返回抓柄矩形（宽高都可能为 0：区间太窄时 ImGui 也不画）。
Rect slider_grab_rect(const Rect& frame, float frac, float grab_min, float grab_round) {
    (void)grab_round;
    const float grab_padding = 2.0f;
    const float slider_sz = frame.w - grab_padding * 2.0f;
    if (slider_sz < 1.0f) return Rect{frame.x, frame.y, 0.0f, 0.0f};
    float grab_sz = (grab_min > 0.0f) ? grab_min : 8.0f;
    if (grab_sz > slider_sz) grab_sz = slider_sz;
    const float usable_min = frame.x + grab_padding + grab_sz * 0.5f;
    const float usable_max = frame.right() - grab_padding - grab_sz * 0.5f;
    const float pos = usable_min + (usable_max - usable_min) * clampf(frac, 0.0f, 1.0f);
    return Rect{pos - grab_sz * 0.5f, frame.y + grab_padding,
                grab_sz, frame.h - grab_padding * 2.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// 颜色编辑的分量框几何（导出：绘制与命中测试共用）
// ---------------------------------------------------------------------------

float ui_color_part_width(const Rect& r) {
    const float w_button = r.h + kItemInnerSpacing;          // 色块 + 间隙
    const float w_inputs = (r.w > w_button) ? (r.w - w_button) : r.w;
    const float w_items  = w_inputs - kItemInnerSpacing * 3.0f;
    const float per = std::floor(w_items / 4.0f);
    return (per > 1.0f) ? per : 1.0f;
}

int ui_color_part_at(const Rect& r, float x) {
    const float per = ui_color_part_width(r);
    for (int i = 0; i < 4; ++i) {
        const float x0 = r.x + static_cast<float>(i) * (per + kItemInnerSpacing);
        if (x >= x0 && x <= x0 + per) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// UTF-8 工具（导出）
// ---------------------------------------------------------------------------

int utf8_text_length(const char* s) {
    return s ? static_cast<int>(std::strlen(s)) : 0;
}

int utf8_prev_boundary(const char* s, int pos) {
    if (!s || pos <= 0) return 0;
    int i = imin(pos, utf8_text_length(s)) - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}

int utf8_next_boundary(const char* s, int pos) {
    if (!s) return 0;
    const int len = utf8_text_length(s);
    if (pos < 0) return 0;
    if (pos >= len) return len;
    return imin(pos + utf8_seq_len(s + pos), len);
}

float text_x_at(const char* s, int byte_pos, const UiTextMetrics& metrics, float font_size) {
    if (!s) return 0.0f;
    const int len = utf8_text_length(s);
    if (byte_pos <= 0) return 0.0f;
    if (byte_pos > len) byte_pos = len;
    if (font_size <= 0.0f) font_size = kWidgetFontSize;
    char buf[kWidgetTextCap * 2 + 1];
    if (byte_pos > static_cast<int>(sizeof(buf)) - 1) byte_pos = static_cast<int>(sizeof(buf)) - 1;
    std::memcpy(buf, s, static_cast<size_t>(byte_pos));
    buf[byte_pos] = '\0';
    return metrics.width(buf, font_size);
}

int text_pos_at(const char* s, float x, const UiTextMetrics& metrics, float font_size) {
    if (!s || x <= 0.0f) return 0;
    if (font_size <= 0.0f) font_size = kWidgetFontSize;
    const int len = utf8_text_length(s);
    char buf[kWidgetTextCap * 2 + 1];

    int   best_pos = 0;
    float best_w   = 0.0f;
    int   pos      = 0;
    while (pos < len) {
        const int n = utf8_seq_len(s + pos);
        const int end = imin(pos + n, len);
        if (end > static_cast<int>(sizeof(buf)) - 1) break;
        std::memcpy(buf, s, static_cast<size_t>(end));
        buf[end] = '\0';
        const float w = metrics.width(buf, font_size);
        if (w <= x) {
            best_pos = end;
            best_w   = w;
        } else {
            // x 落在 [best_w, w] 之间：谁近算谁
            if ((x - best_w) > (w - x)) best_pos = end;
            return best_pos;
        }
        pos = end;
    }
    return best_pos;
}

namespace {

inline bool is_word_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

} // namespace

int text_word_left(const char* s, int pos) {
    if (!s || pos <= 0) return 0;
    const int len = utf8_text_length(s);
    if (pos > len) pos = len;
    int i = pos;
    // 先跳过左侧的非词字符，再吞掉一段词
    while (i > 0) {
        const int p = utf8_prev_boundary(s, i);
        if (is_word_char(static_cast<unsigned char>(s[p]))) break;
        i = p;
    }
    while (i > 0) {
        const int p = utf8_prev_boundary(s, i);
        if (!is_word_char(static_cast<unsigned char>(s[p]))) break;
        i = p;
    }
    return i;
}

int text_word_right(const char* s, int pos) {
    if (!s) return 0;
    const int len = utf8_text_length(s);
    if (pos < 0) pos = 0;
    int i = pos;
    while (i < len) {
        if (!is_word_char(static_cast<unsigned char>(s[i]))) break;
        i = utf8_next_boundary(s, i);
    }
    while (i < len) {
        if (is_word_char(static_cast<unsigned char>(s[i]))) break;
        i = utf8_next_boundary(s, i);
    }
    return i;
}

// ---------------------------------------------------------------------------
// ui_draw_widget — 控件绘制
// ---------------------------------------------------------------------------

void ui_draw_widget(const Widget& w, const Theme& theme, const UiTextMetrics& metrics,
                    uint64_t focus_id, UiDrawList& dl) {
    ui_draw_widget(w, theme, metrics, UiDrawContext{focus_id, true}, dl);
}

void ui_draw_widget(const Widget& w, const Theme& theme, const UiTextMetrics& metrics,
                    const UiDrawContext& ctx, UiDrawList& dl) {
    const Rect&  r  = w.rect;
    const float  fs = kWidgetFontSize;
    const float  lh = metrics.line_height(fs);
    const float  ty = r.y + (r.h - lh) * 0.5f;   // 行盒在控件里垂直居中
    const float  bw = theme_metric(theme, THEME_METRIC_BORDER_W, 1.0f);
    const bool   focused = (w.id == ctx.focus_id);
    const uint32_t text_col = w.text_overridden
                            ? w.text_color
                            : theme_color(theme, w.enabled ? THEME_COLOR_TEXT : THEME_COLOR_TEXT_DIM);

    // 角半径：控件自己指定优先（< 0 = 主题默认）。窗口/弹层/控件在主题里是
    // 三档不同的值——全都用同一个半径的话，"面板 6px、里面的按钮也 6px"会
    // 让层次看起来一样厚，面板不像面板。
    const float r_frame  = (w.corner_radius >= 0.0f) ? w.corner_radius
                         : theme_metric(theme, THEME_METRIC_FRAME_ROUNDING, 4.0f);
    const float r_window = (w.corner_radius >= 0.0f) ? w.corner_radius
                         : theme_metric(theme, THEME_METRIC_WINDOW_ROUNDING, 6.0f);
    const float r_popup  = (w.corner_radius >= 0.0f) ? w.corner_radius
                         : theme_metric(theme, THEME_METRIC_POPUP_ROUNDING, 4.0f);
    const float r_child  = (w.corner_radius >= 0.0f) ? w.corner_radius
                         : theme_metric(theme, THEME_METRIC_CHILD_ROUNDING, 4.0f);
    const float r_grab   = theme_metric(theme, THEME_METRIC_GRAB_ROUNDING, 3.0f);

    // 投影：主题 token 存的是不透明的暗色，直接画就是一块灰板，alpha 在这里压低。
    // 分两层——贴身的紧影给出"离地关系"，外围的散影给出"悬空体积"；单层要么
    // 糊成一团灰晕，要么像贴纸。强度与扩散由主题 style 块决定：Fluent 要大而
    // 柔的环境光，Blender 的编辑区面板平贴、几乎不要影子（shadow_alpha 归零
    // 就连紧影也不画）。
    const ThemeStyle& st = theme.style;
    const uint32_t shadow_rgb = theme_color(theme, THEME_COLOR_BORDER_SHADOW) & 0xFFFFFF00u;
    const float    shadow_a   = (st.shadow_alpha < 0.0f ? 0.0f : (st.shadow_alpha > 1.0f ? 1.0f : st.shadow_alpha));
    const uint32_t shadow_near = shadow_rgb | static_cast<uint8_t>(shadow_a * 191.0f + 0.5f);
    const uint32_t shadow_far  = shadow_rgb | static_cast<uint8_t>(shadow_a * 102.0f + 0.5f);
    const float    spread_near = st.shadow_spread * 0.35f;
    const float    spread_far  = st.shadow_spread;
    const bool     want_shadow = (w.shadow && shadow_a > 0.01f);

    switch (w.kind) {
        case WidgetKind::Label:
            dl.add_text(r.x, ty, w.label, fs, text_col);
            break;

        case WidgetKind::Button: {
            // 与 ImGui 的 Button 一致：纯色填充 + 1px 边框 + 居中标签。
            // 状态色只换填充（BUTTON → BUTTON_HOVER → BUTTON_ACTIVE），
            // 边框始终是 BORDER——悬停时改边框色会让按钮"跳一下"。
            const uint32_t bg = w.bg_overridden ? w.bg_color : widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, bg,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            const float tw = metrics.width(w.label, fs);
            dl.add_text(r.x + (r.w - tw) * 0.5f, ty, w.label, fs, text_col);
            break;
        }

        case WidgetKind::Panel: {
            // 弹层（layer 1）用弹层圆角、窗口用窗口圆角：两者的视觉权重不同
            const float rad = (w.layer > 0) ? r_popup : r_window;
            if (want_shadow) {
                dl.add_shadow(r.x, r.y, r.w, r.h, rad, spread_near, shadow_near);
                dl.add_shadow(r.x, r.y, r.w, r.h, rad, spread_far, shadow_far);
            }
            const uint32_t base = w.bg_overridden
                                ? w.bg_color
                                : theme_color(theme, THEME_COLOR_WINDOW_BG);
            // 渐变幅度由主题 style 决定：出厂 0（与 ImGui 的纯色窗口一致），
            // JSON 主题要质感时才有上下差。
            draw_round_frame_gradient(dl, r, rad, shade(base, st.panel_grad),
                                      shade(base, -st.panel_grad * 0.5f),
                                      theme_color(theme, THEME_COLOR_BORDER), bw);
            break;
        }

        case WidgetKind::Separator:
            // ImGui 的 Separator 就是一条 1px 的 SEPARATOR 色横线，不混底色
            dl.add_rect(r.x, r.y, r.w, 1.0f, theme_color(theme, THEME_COLOR_SEPARATOR));
            break;

        case WidgetKind::Progress: {
            // ImGui 的 ProgressBar：框底（FrameBg + 边框）+ 按比例填充的
            // PLOT_HISTOGRAM 色条 + 居中的浮层文字。
            const uint32_t base = widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, base,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            const float frac = clampf(w.value, 0.0f, 1.0f);
            if (frac > 0.0f && r.w > bw * 2.0f && r.h > bw * 2.0f) {
                float inner_r = clamp_radius(r_frame - bw, r);
                const float fill_h = r.h - bw * 2.0f;
                if (inner_r > fill_h * 0.5f) inner_r = fill_h * 0.5f;
                const uint32_t a = theme_color(theme, THEME_COLOR_PLOT_HISTOGRAM);
                dl.add_round_rect(r.x + bw, r.y + bw, (r.w - bw * 2.0f) * frac,
                                  fill_h, inner_r, a);
            }
            if (w.label[0]) {
                draw_text_centered(dl, r, w.label, fs, metrics,
                                   theme_color(theme, THEME_COLOR_TEXT));
            }
            break;
        }

        case WidgetKind::Checkbox: {
            // ImGui 的 Checkbox：左边一个"框高见方"的方框（不是固定 16px），
            // 勾选时框底换 CHECKBOX_BG、再叠一个 CheckMark 色的勾，标签在框右。
            const float side = r.h;
            const Rect  box{r.x, r.y, side, side};
            const uint32_t box_bg = w.checked
                                  ? theme_color(theme, THEME_COLOR_CHECKBOX_BG)
                                  : widget_fill_now(w, theme);
            draw_frame_box(dl, box, r_frame, box_bg,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            if (w.checked) {
                draw_check_mark(dl, box, theme_color(theme, THEME_COLOR_CHECK_MARK));
            }
            if (w.label[0]) {
                dl.add_text(box.right() + kItemInnerSpacing, ty, w.label, fs,
                            theme_color(theme, w.enabled ? THEME_COLOR_TEXT : THEME_COLOR_TEXT_DIM));
            }
            break;
        }

        case WidgetKind::Slider: {
            // ImGui 的 SliderFloat：**框既是底也是轨道**，数值画在框内居中，
            // 抓柄是一条 8px 宽的圆角竖条（不是"轨道上的胶囊"），标签在框右。
            const uint32_t bg = widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, bg,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            const float span = (w.max_value > w.min_value) ? (w.max_value - w.min_value) : 1.0f;
            const float frac = clampf((w.value - w.min_value) / span, 0.0f, 1.0f);

            char val[32];
            format_widget_value(val, sizeof(val), w.fmt, w.value);
            draw_text_centered(dl, r, val, fs, metrics, theme_color(theme, THEME_COLOR_TEXT));

            const Rect grab = slider_grab_rect(
                r, frac, theme_metric(theme, THEME_METRIC_GRAB_MIN_SIZE, 8.0f),
                theme_metric(theme, THEME_METRIC_GRAB_ROUNDING, 4.0f));
            if (grab.w > 0.0f) {
                dl.add_round_rect(grab.x, grab.y, grab.w, grab.h, r_grab,
                                  theme_color(theme, w.pressed ? THEME_COLOR_SLIDER_GRAB_ACTIVE
                                                               : THEME_COLOR_SLIDER_GRAB));
            }
            draw_label_right(dl, w, metrics, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::DragFloat: {
            // 与 ImGui 的 DragFloat 一致：一条可拖的"值框"，值居中、标签在框右。
            const uint32_t bg = widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, bg,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            char val[32];
            format_widget_value(val, sizeof(val), w.fmt, w.value);
            draw_text_centered(dl, r, val, fs, metrics, theme_color(theme, THEME_COLOR_TEXT));
            draw_label_right(dl, w, metrics, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::Combo: {
            // ImGui 的 Combo：框内左侧当前项 + 框右侧的箭头按钮，标签在框右。
            // 两半同色（FrameBg / Button 在本主题里是同一个 token），所以视觉上
            // 就是一整条框；箭头部分单独画是为了 hover 时能只亮右半边。
            const float arrow_sz = (r.h < fs ? r.h : fs * 1.0f) + 6.0f;
            const float arrow_w = (arrow_sz < r.w) ? arrow_sz : r.w;
            const uint32_t bg = widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, bg,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            // 预览文字左对齐、贴着框的内边距（ImGui: bb.Min + FramePadding）
            dl.push_clip(r.x, r.y, r.w - arrow_w, r.h);
            dl.add_text(r.x + 8.0f, ty, w.text, fs, theme_color(theme, THEME_COLOR_TEXT));
            dl.pop_clip();
            draw_arrow_down(dl, r.right() - arrow_w + 4.0f, r.y + (r.h - fs) * 0.5f, fs,
                            theme_color(theme, THEME_COLOR_TEXT));
            draw_label_right(dl, w, metrics, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::ColorEdit: {
            // ImGui 的 ColorEdit4：4 个分量框（左缘一条通道色标）+ 色块，标签在右。
            // 分量宽度按 CalcItemWidth 扣掉色块后四等分——与 ImGui 同一个算法，
            // 所以四个框的右边缘和色块位置逐像素对齐。
            const float sq = r.h;                       // square_sz = 框高
            const float w_button = sq + kItemInnerSpacing;
            const float w_inputs = (r.w > w_button) ? (r.w - w_button) : r.w;
            const float per = ui_color_part_width(r);
            static const uint32_t kMarker[4] = {0xFF0000FFu, 0x00FF00FFu,
                                                0x0000FFFFu, 0xFFFFFFFFu};
            const uint32_t bg = widget_fill_now(w, theme);
            float x = r.x;
            for (int i = 0; i < 4; ++i) {
                const Rect box{x, r.y, per, r.h};
                draw_frame_box(dl, box, r_frame, bg,
                               theme_color(theme, THEME_COLOR_BORDER), bw);
                // 通道色标：框左缘 2px 宽的竖条（ImGui 的 ColorMarkerSize）
                if (per > 4.0f) {
                    dl.add_round_rect(box.x + 1.0f, box.y + 1.0f, 2.0f, box.h - 2.0f,
                                      1.0f, kMarker[i]);
                }
                char val[16];
                std::snprintf(val, sizeof(val), "%.3f", w.color[i]);
                draw_text_centered(dl, box, val, fs, metrics,
                                   theme_color(theme, THEME_COLOR_TEXT));
                x += per + kItemInnerSpacing;
            }
            // 色块：本身颜色 + 1px 边框（ImGui 的 ColorButton）
            const Rect sw{r.x + w_inputs + kItemInnerSpacing, r.y, sq, r.h};
            const uint32_t rgba = (static_cast<uint32_t>(clampf(w.color[0], 0.0f, 1.0f) * 255.0f + 0.5f) << 24)
                                | (static_cast<uint32_t>(clampf(w.color[1], 0.0f, 1.0f) * 255.0f + 0.5f) << 16)
                                | (static_cast<uint32_t>(clampf(w.color[2], 0.0f, 1.0f) * 255.0f + 0.5f) << 8)
                                | static_cast<uint32_t>(clampf(w.color[3], 0.0f, 1.0f) * 255.0f + 0.5f);
            draw_frame_box(dl, sw, r_frame, rgba,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            draw_label_right(dl, w, metrics, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::TreeNode: {
            draw_fold_marker(dl, r.x + 2.0f, ty, w.open, theme, metrics, fs);
            const float ox = r.x + 4.0f + fs * 0.75f + 4.0f;
            dl.add_text(ox, ty, w.label, fs,
                        w.hovered ? theme_color(theme, THEME_COLOR_TEXT)
                                  : theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::Header: {
            const uint32_t bg = widget_fill_now(w, theme);
            draw_round_frame_gradient(dl, r, r_frame,
                                      shade(bg, st.panel_grad * 1.5f),
                                      shade(bg, -st.panel_grad * 0.5f), 0u, 0.0f);
            draw_fold_marker(dl, r.x + 4.0f, ty, w.open, theme, metrics, fs);
            const float ox = r.x + 8.0f + fs * 0.75f + 2.0f;
            dl.add_text(ox, ty, w.label, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::MenuItem: {
            // 高亮块带淡入/淡出（动画关闭时 alpha 瞬变，行为与旧版一致）
            const uint32_t hl = widget_fill_now(w, theme);
            if ((hl & 0xFFu) != 0) {
                dl.add_round_rect(r.x, r.y, r.w, r.h, r_frame, hl);
            }
            dl.add_text(r.x + 8.0f, ty, w.label, fs,
                        theme_color(theme, w.enabled ? THEME_COLOR_TEXT : THEME_COLOR_TEXT_DIM));
            if (w.text[0]) {
                const float sw = metrics.width(w.text, fs);
                dl.add_text(r.right() - sw - 8.0f, ty, w.text, fs,
                            theme_color(theme, THEME_COLOR_TEXT_DIM));
            }
            break;
        }

        case WidgetKind::Selectable: {
            // 列表项：底色（选中/悬停）+ 左对齐文本。文本左对齐而不是居中——
            // 列表里一列文字要对齐才好扫读。
            const uint32_t hl = widget_fill_now(w, theme);
            if ((hl & 0xFFu) != 0) {
                dl.add_round_rect(r.x, r.y, r.w, r.h, r_frame, hl);
            }
            dl.add_text(r.x + 8.0f, ty, w.label, fs, text_col);
            break;
        }

        case WidgetKind::Image: {
            // 图片：直接走 drawlist 的 sprite 通道（UiCmd::Sprite），
            // 由 ui_render_bridge 交给 Renderer2D::draw_sprite_region 采样。
            // uv 在建控件时就按后端采样约定算好（见 BuiltinUi::image），
            // 绘制端不再判断后端。纹理句柄为 0 时跳过（调用方给了空纹理）。
            if (w.texture != 0) {
                dl.add_sprite(r.x, r.y, r.w, r.h, w.texture,
                              w.u0, w.v0, w.u1, w.v1, 0xFFFFFFFFu);
            }
            break;
        }

        case WidgetKind::ScrollBar: {
            // rect 就是轨道本身（调用方给出窄条，命中测试才只落在条上）
            dl.add_round_rect(r.x, r.y, r.w, r.h, r.w * 0.5f,
                              theme_color(theme, THEME_COLOR_SCROLLBAR_BG));
            const float frac = clampf(w.value, 0.0f, 1.0f);
            const float vis  = clampf(w.thumb, 0.05f, 1.0f);
            const float thumb_h = r.h * vis;
            const float max_off = r.h - thumb_h;
            const float thumb_y = r.y + max_off * frac;
            const uint32_t tc = w.pressed ? theme_color(theme, THEME_COLOR_SCROLLBAR_GRAB_ACTIVE)
                              : w.hovered ? theme_color(theme, THEME_COLOR_SCROLLBAR_GRAB_HOVERED)
                                          : theme_color(theme, THEME_COLOR_SCROLLBAR_GRAB);
            const float tw2 = (r.w > 4.0f) ? (r.w - 4.0f) : r.w;
            dl.add_round_rect(r.x + 2.0f, thumb_y, tw2, thumb_h, tw2 * 0.5f, tc);
            break;
        }

        case WidgetKind::Bar:
            dl.add_rect(r.x, r.y, r.w, r.h,
                        w.bg_overridden ? w.bg_color : theme_color(theme, THEME_COLOR_MENUBAR_BG));
            dl.add_rect(r.x, r.bottom() - 1.0f, r.w, 1.0f,
                        mix_rgba(theme_color(theme, THEME_COLOR_BORDER),
                                 theme_color(theme, THEME_COLOR_MENUBAR_BG), 0.4f));
            break;

        case WidgetKind::Frame:
            if (want_shadow) dl.add_shadow(r.x, r.y, r.w, r.h, r_child, spread_far * 0.6f, shadow_far);
            {
                const uint32_t base = w.bg_overridden
                                    ? w.bg_color
                                    : theme_color(theme, THEME_COLOR_CHILD_BG);
                // 描边就是 BORDER 本色：不要往子区域底色里混——混过之后边框是
                // #333333 而不是 #383838，与窗口/控件的边框不在同一档灰度上，
                // 子区域的边看起来比窗口边"虚"。
                draw_round_frame_gradient(dl, r, r_child, shade(base, st.panel_grad),
                                          shade(base, -st.panel_grad * 0.5f),
                                          theme_color(theme, THEME_COLOR_BORDER),
                                          bw);
            }
            break;

        case WidgetKind::TitleBar: {
            // ImGui 的标题栏：TITLE_BG_ACTIVE 纯色（上圆下方）、标题文字在
            // 左内边距处、底部压一条 1px 的 BORDER 线把标题与内容分开。
            const uint32_t bg = !w.enabled ? theme_color(theme, THEME_COLOR_TITLE_BG_COLLAPSED)
                                           : theme_color(theme, THEME_COLOR_TITLE_BG_ACTIVE);
            const float tradii[4] = {r_window, r_window, 0.0f, 0.0f};
            // 渐变幅度跟着面板档（出厂 0 = 与 ImGui 同一个纯色）
            dl.add_round_gradient4(r.x, r.y, r.w, r.h, tradii,
                                   shade(bg, st.panel_grad * 2.5f),
                                   shade(bg, -st.panel_grad));
            // 标题栏自己描一圈边：窗口底板的描边先画、被标题栏盖住，不补这一条
            // 时窗口上沿没有边框，圆角处还会露出一像素底板色。下沿那条边同时
            // 充当"标题栏 / 内容区"的分隔线。
            dl.add_round_border4(r.x, r.y, r.w, r.h, tradii, bw,
                                 theme_color(theme, THEME_COLOR_BORDER));
            // 折叠框只在窗口允许折叠时画（ImGui 的 NoCollapse 同理）
            float text_x = r.x + 8.0f;
            if (w.fold_marker) {
                draw_fold_marker(dl, r.x + 6.0f, r.y + (r.h - fs) * 0.5f, w.open,
                                 theme, metrics, fs);
                text_x += fs + kItemInnerSpacing;
            }
            dl.add_text(text_x, r.y + (r.h - fs) * 0.5f, w.label, fs,
                        theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::Tooltip: {
            if (want_shadow) {
                dl.add_shadow(r.x, r.y, r.w, r.h, r_popup, spread_near, shadow_near);
                dl.add_shadow(r.x, r.y, r.w, r.h, r_popup, spread_far, shadow_far);
            }
            const uint32_t tbg = theme_color(theme, THEME_COLOR_POPUP_BG);
            draw_round_frame_gradient(dl, r, r_popup, shade(tbg, st.panel_grad * 1.5f),
                                      shade(tbg, -st.panel_grad * 0.5f),
                                      theme_color(theme, THEME_COLOR_BORDER), bw);
            dl.add_text(r.x + 6.0f, ty, w.label, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::TextInput: {
            // 与 ImGui 的 InputText 一致：框 + 框内左对齐正文（内边距 = 8）
            // + 右侧标签。原先"反向渐变 + 更沉的底色"是为了在没有标签位置
            // 差异时让人看出哪里能打字；现在标签在右、框宽固定，形态本身就是
            // 输入框的样子，反向渐变反而与 ImGui 不一致。
            const uint32_t base = widget_fill_now(w, theme);
            draw_frame_box(dl, r, r_frame, base,
                           theme_color(theme, THEME_COLOR_BORDER), bw);
            // 长文本不许淌出框外：裁剪 + 只画框内那一段
            dl.push_clip(r.x + 1.0f, r.y, r.w - 2.0f, r.h);
            const float tx = r.x + 8.0f - w.scroll_x;
            const int   len = utf8_text_length(w.text);

            // 选区（只在该控件有焦点时画：失焦后不显示残留高亮）
            if (focused && w.sel_anchor >= 0 && w.sel_anchor != w.caret) {
                const int a = imin(w.sel_anchor, w.caret);
                const int b = imax(w.sel_anchor, w.caret);
                const float x0 = tx + text_x_at(w.text, a, metrics, fs);
                const float x1 = tx + text_x_at(w.text, b, metrics, fs);
                if (x1 > x0) {
                    dl.add_rect(x0, r.y + (r.h - lh) * 0.5f, x1 - x0, lh,
                                theme_color(theme, THEME_COLOR_SELECTION));
                }
            }

            dl.add_text(tx, ty, w.text, fs, theme_color(theme, THEME_COLOR_TEXT));

            // 插入符 + 输入法预编辑（预编辑画在插入符之后，带下划线）
            const float cx = tx + text_x_at(w.text, clampint(w.caret, 0, len), metrics, fs);
            if (focused && ctx.caret_visible) {
                dl.add_rect(cx, r.y + (r.h - lh) * 0.5f + 1.0f, 1.0f, lh - 2.0f,
                            theme_color(theme, THEME_COLOR_TEXT));
            }
            if (focused && w.preedit[0]) {
                dl.add_text(cx, ty, w.preedit, fs, theme_color(theme, THEME_COLOR_TEXT));
                const float pw = metrics.width(w.preedit, fs);
                dl.add_rect(cx, r.y + (r.h + lh) * 0.5f - 2.0f, pw, 1.0f,
                            theme_color(theme, THEME_COLOR_ACCENT));
            }
            dl.pop_clip();
            draw_label_right(dl, w, metrics, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::ClipBegin:
        case WidgetKind::ClipEnd:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// WidgetTree
// ---------------------------------------------------------------------------

void WidgetTree::begin_frame(const UiInputFrame& input, const UiTextMetrics* metrics) {
    input_   = input;
    metrics_ = metrics;
    ++frame_;
    seq_next_ = 1;
    press_claimed_ = 0;

    // 插入符闪烁：宿主给了时钟才闪（0 = 恒显，截图与单测可复现）
    if (input.time > 0.0f) {
        const float phase = input.time - static_cast<float>(static_cast<int>(input.time / 1.06f)) * 1.06f;
        caret_visible_ = phase < 0.53f;
    } else {
        caret_visible_ = true;
    }
}

Widget* WidgetTree::ensure(uint64_t id, WidgetKind kind, const Rect& rect, const char* label) {
    // 0 是"无控件"哨兵（与 ui_id.h 的约定一致），不接受
    if (id == 0) return nullptr;
    Slot* s = find_slot(id);
    if (!s) {
        s = alloc(id);
        if (!s) return nullptr;   // 表满：调用方应当看见失败
    }
    Widget& w = s->w;
    w.kind = kind;
    w.rect = rect;
    w.seq  = seq_next_++;   // 同帧重复提交（ensure→touch）以后一次为准
    w.has_clip = clip_active_;
    w.clip = pending_clip_;
    w.label[0] = '\0';
    if (label) {
        std::strncpy(w.label, label, sizeof(w.label) - 1);
        w.label[sizeof(w.label) - 1] = '\0';
    }
    s->frame = frame_;
    return &w;
}

Widget* WidgetTree::touch(uint64_t id, WidgetKind kind, const Rect& rect, const char* label) {
    Widget* w = ensure(id, kind, rect, label);
    if (!w) return nullptr;

    // 每帧标志先清零。命中测试只问"这个 kind 该不该参与"（widget_is_interactive）：
    // 面板背景、分隔线、提示、裁剪命令一类的大矩形/伪控件因此不会抢走覆盖在
    // 它们上面的按钮。分类表见 ui_widget.h 的 WidgetClass。
    w->hovered = false;
    w->pressed = false;
    w->clicked = false;
    // 提交后马上查询（ImGui 的用法）要求这一帧就看得见；而"点了后面的控件
    // 才失焦"要能活到下一帧，所以按帧号放两帧，超过就认为宿主已经查过了。
    if (w->deactivated_after_edit && frame_ > w->deactivated_frame + 1) {
        w->deactivated_after_edit = false;
        w->deactivated_frame = 0;
    }
    if (!w->enabled || !widget_is_interactive(kind)) {
        return w;
    }

    bool inside = rect_contains(rect, input_.mouse_x, input_.mouse_y);
    if (inside && w->has_clip && !rect_contains(w->clip, input_.mouse_x, input_.mouse_y)) {
        inside = false;   // 滚出可见区 / 超出弹层：看得见才点得中
    }
    if (inside) w->hovered = true;

    if (inside && input_.mouse_clicked[0]) {
        // 同一帧里后提交的包含鼠标的控件拿到 active（提交序 = 上层序），
        // 重叠时上层拿按下。
        active_id_ = id;
        press_claimed_ = id;
        if (kind == WidgetKind::TextInput) {
            focus_id_ = id;
            // 点击定位插入点：鼠标 x → 字节偏移（用与绘制同一份度量）
            if (metrics_) {
                const float tx = input_.mouse_x - (rect.x + 4.0f) + w->scroll_x;
                w->caret = text_pos_at(w->text, tx, *metrics_, kWidgetFontSize);
                w->sel_anchor = w->caret;   // 空选区；拖动时拉开
            }
        } else if (focus_id_ != 0) {
            drop_focus();    // 点别的控件：焦点转移走（走统一出口，记得住"改过"）
        }
    }
    if (active_id_ == id && input_.mouse_down[0]) {
        w->pressed = true;
        // 输入框里按住拖动 = 拖选（锚点已在按下帧落好）
        if (kind == WidgetKind::TextInput && metrics_ && w->sel_anchor >= 0) {
            const float tx = input_.mouse_x - (rect.x + 4.0f) + w->scroll_x;
            w->caret = text_pos_at(w->text, tx, *metrics_, kWidgetFontSize);
        }
    }
    // 拖到外面松开 = 取消，不产生点击
    if (active_id_ == id && input_.mouse_released[0] && inside) w->clicked = true;

    apply_standard_interaction(*w);
    return w;
}

void WidgetTree::apply_standard_interaction(Widget& w) {
    switch (w.kind) {
        case WidgetKind::Checkbox:
            if (w.clicked) w.checked = !w.checked;
            break;

        case WidgetKind::Slider: {
            if (!w.pressed) break;
            const float span = w.max_value - w.min_value;
            if (span <= 0.0f || w.rect.w <= 0.0f) break;
            // 按住即拖：鼠标横向位置映射到 [min,max]（拖出框外也钳住继续拖）
            const float t = clampf((input_.mouse_x - w.rect.x) / w.rect.w, 0.0f, 1.0f);
            w.value = w.min_value + t * span;
            break;
        }

        case WidgetKind::DragFloat: {
            // 增量式：按住时按鼠标位移改值（没有"点哪算哪"的语义，
            // 所以第一次按下要记下基准，之后每帧只加位移）。
            if (w.clicked) w.value = w.value;   // 点击不改值（只捕获拖拽）
            if (w.pressed) {
                const float dx = input_.mouse_x - drag_last_x_;
                if (drag_owner_ == w.id) {
                    const float speed = drag_speed_;
                    const float step = (input_.mods & UiInputFrame::ModCtrl) ? speed * 0.1f : speed;
                    w.value += dx * step;
                    if (w.max_value > w.min_value) {
                        w.value = clampf(w.value, w.min_value, w.max_value);
                    }
                }
            }
            if (w.pressed || w.clicked) {
                drag_owner_ = w.id;
                drag_speed_ = drag_speed_hint_;
            }
            break;
        }

        case WidgetKind::TreeNode:
        case WidgetKind::Header:
            if (w.clicked) w.open = !w.open;
            break;

        case WidgetKind::ColorEdit: {
            // ImGui 的 ColorEdit4：按下哪个分量框就拖哪个，每像素 1/255。
            // 分量框宽度与绘制共用 ui_color_part_at，点哪儿改哪儿。
            if (!w.pressed) {
                w.color_part = -1;
                break;
            }
            if (drag_owner_ != w.id) w.color_part = ui_color_part_at(w.rect, input_.mouse_x);
            if (w.color_part >= 0 && w.color_part < 4 && drag_owner_ == w.id) {
                const float dx = input_.mouse_x - drag_last_x_;
                w.color[w.color_part] =
                    clampf(w.color[w.color_part] + dx * (1.0f / 255.0f), 0.0f, 1.0f);
            }
            drag_owner_ = w.id;
            drag_speed_ = 1.0f / 255.0f;
            break;
        }

        case WidgetKind::ScrollBar: {
            if (!w.pressed || w.rect.h <= 0.0f) break;
            const float vis = clampf(w.thumb, 0.05f, 1.0f);
            // 点击轨道：把滑块中心对到鼠标（ImGui 的"跳转"语义）
            const float max_off = w.rect.h * (1.0f - vis);
            const float frac = (max_off <= 0.0f)
                             ? 0.0f
                             : clampf((input_.mouse_y - w.rect.y - w.rect.h * vis * 0.5f) / max_off,
                                      0.0f, 1.0f);
            w.value = frac;
            break;
        }

        case WidgetKind::TextInput: {
            // 只有聚焦的那个框会被编辑；记下"这一激活期内改过内容"，失焦时才
            // 判得出该不该算一次提交（IsItemDeactivatedAfterEdit 的语义）。
            // 比对整串而不是在每个编辑分支里打标记：编辑入口有七八处（插入、
            // 删除、粘贴、IME 上屏…），逐个打必然漏。
            if (focus_id_ == w.id) {
                char keep[kWidgetTextCap];
                std::snprintf(keep, sizeof(keep), "%s", w.text);
                edit_text(w);
                if (std::strcmp(keep, w.text) != 0) w.edited_while_focused = true;
            } else {
                w.preedit[0] = '\0';
            }
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 文本编辑
// ---------------------------------------------------------------------------

namespace {

int text_len(const Widget& w) { return utf8_text_length(w.text); }

void text_erase(Widget& w, int from, int to) {
    const int len = text_len(w);
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to <= from) return;
    std::memmove(w.text + from, w.text + to, static_cast<size_t>(len - to) + 1);
}

// 在插入点插入 n 个字节（调用方保证已清掉选区）
void text_insert_raw(Widget& w, const char* s, int n) {
    if (n <= 0) return;
    const int len = text_len(w);
    if (len + n >= kWidgetTextCap) return;   // 放不下整段就整段丢弃，不截断半个码点
    std::memmove(w.text + w.caret + n, w.text + w.caret,
                 static_cast<size_t>(len - w.caret) + 1);
    std::memcpy(w.text + w.caret, s, static_cast<size_t>(n));
    w.caret += n;
}

} // namespace

void WidgetTree::sync_text_scroll(Widget& w) const {
    if (!metrics_) return;
    const float fs = kWidgetFontSize;
    const float cx = text_x_at(w.text, clampint(w.caret, 0, text_len(w)), *metrics_, fs);
    const float avail = w.rect.w - 8.0f;   // 左右各 4px 内边距
    if (avail <= 0.0f) {
        w.scroll_x = 0.0f;
        return;
    }
    if (cx - w.scroll_x > avail) w.scroll_x = cx - avail;
    if (cx - w.scroll_x < 0.0f) w.scroll_x = cx;
    if (w.scroll_x < 0.0f) w.scroll_x = 0.0f;
}

void WidgetTree::edit_text(Widget& w) {
    if (focus_id_ != w.id) {
        w.preedit[0] = '\0';   // 失焦：预编辑不该留在控件上
        return;
    }

    const int  len   = text_len(w);
    const bool shift = (input_.mods & UiInputFrame::ModShift) != 0;
    const bool ctrl  = (input_.mods & UiInputFrame::ModCtrl) != 0;

    // 归一化（宿主可能直接改 text，caret 会越界）
    if (w.caret < 0) w.caret = 0;
    if (w.caret > len) w.caret = len;
    if (w.sel_anchor > len) w.sel_anchor = len;
    if (w.sel_anchor < 0) w.sel_anchor = -1;

    auto has_selection = [&]() { return w.sel_anchor >= 0 && w.sel_anchor != w.caret; };
    auto sel_range = [&](int* a, int* b) {
        *a = imin(w.sel_anchor, w.caret);
        *b = imax(w.sel_anchor, w.caret);
    };
    auto move_to = [&](int to, bool extend) {
        if (to < 0) to = 0;
        if (to > len) to = len;
        if (extend) {
            if (w.sel_anchor < 0) w.sel_anchor = w.caret;
        } else {
            w.sel_anchor = -1;
        }
        w.caret = to;
    };
    auto delete_selection = [&]() {
        int a = 0, b = 0;
        sel_range(&a, &b);
        text_erase(w, a, b);
        w.caret = a;
        w.sel_anchor = -1;
    };
    auto delete_back = [&]() {
        if (has_selection()) { delete_selection(); return; }
        if (w.caret <= 0) return;
        const int p = utf8_prev_boundary(w.text, w.caret);
        text_erase(w, p, w.caret);
        w.caret = p;
        w.sel_anchor = -1;
    };
    auto delete_forward = [&]() {
        if (has_selection()) { delete_selection(); return; }
        if (w.caret >= len) return;
        const int n = utf8_next_boundary(w.text, w.caret);
        text_erase(w, w.caret, n);
        w.sel_anchor = -1;
    };

    // ---- 编辑键（按按下顺序处理）----
    for (int i = 0; i < input_.key_count; ++i) {
        const int k = input_.keys[i];
        switch (k) {
            case UiInputFrame::KeyLeft:
                move_to(ctrl ? text_word_left(w.text, w.caret)
                             : utf8_prev_boundary(w.text, w.caret), shift);
                break;
            case UiInputFrame::KeyRight:
                move_to(ctrl ? text_word_right(w.text, w.caret)
                             : utf8_next_boundary(w.text, w.caret), shift);
                break;
            case UiInputFrame::KeyHome: move_to(0, shift); break;
            case UiInputFrame::KeyEnd:  move_to(len, shift); break;
            case UiInputFrame::KeyDelete: delete_forward(); break;
            case UiInputFrame::KeyBackspace: delete_back(); break;
            case UiInputFrame::KeyEsc:
                // Esc = 放弃这次编辑：先丢掉"改过"标记，失焦就不算提交。
                w.sel_anchor = -1;
                w.preedit[0] = '\0';
                w.edited_while_focused = false;
                drop_focus();
                break;
            case UiInputFrame::KeyEnter:
                // 回车 = 提交并失焦（ImGui 的默认行为）。宿主的"失焦即提交"
                // 因此对回车与点别处是同一条路径，不用各写一遍。
                drop_focus();
                break;
            default:
                break;
        }
    }
    // 兼容旧的布尔退格字段（宿主没填 keys[] 时也得能用）
    if (input_.backspace && !input_.key_pressed(UiInputFrame::KeyBackspace)) {
        delete_back();
    }

    // ---- 剪贴板 ----
    if (ctrl && !(input_.mods & UiInputFrame::ModAlt)) {
        UiClipboard& clip = ui_clipboard();
        if (input_.key_pressed(UiInputFrame::KeyA)) {
            w.sel_anchor = 0;
            w.caret = len;
        }
        if (input_.key_pressed(UiInputFrame::KeyC) || input_.key_pressed(UiInputFrame::KeyX)) {
            if (has_selection()) {
                int a = 0, b = 0;
                sel_range(&a, &b);
                const int n = imin(b - a, static_cast<int>(sizeof(clip.text)) - 1);
                std::memcpy(clip.text, w.text + a, static_cast<size_t>(n));
                clip.text[n] = '\0';
                ++clip.seq;
                if (input_.key_pressed(UiInputFrame::KeyX)) delete_selection();
            }
        }
        if (input_.key_pressed(UiInputFrame::KeyV)) {
            if (has_selection()) delete_selection();
            const int n = imin(utf8_text_length(clip.text),
                               kWidgetTextCap - 1 - text_len(w));
            if (n > 0) text_insert_raw(w, clip.text, n);
        }
    }

    // ---- 字符输入（覆盖选区）----
    if (input_.char_count > 0) {
        if (has_selection()) delete_selection();
        for (int i = 0; i < input_.char_count; ++i) {
            const uint32_t cp = input_.chars[i];
            if (cp < 0x20u || cp == 0x7Fu) continue;   // 控制字符不进输入框
            char tmp[kWidgetTextCap];
            tmp[0] = '\0';
            utf8_append(tmp, sizeof(tmp), cp);
            text_insert_raw(w, tmp, static_cast<int>(std::strlen(tmp)));
        }
        w.sel_anchor = -1;
    }

    // ---- 预编辑（输入法组合态）----
    std::snprintf(w.preedit, sizeof(w.preedit), "%s", input_.preedit);

    // 重新归一化并在需要时横向滚动，保证插入点可见
    if (w.caret < 0) w.caret = 0;
    if (w.caret > text_len(w)) w.caret = text_len(w);
    if (w.sel_anchor > text_len(w)) w.sel_anchor = text_len(w);
    sync_text_scroll(w);
}

void WidgetTree::set_clip(const Rect& r) {
    clip_active_ = true;
    pending_clip_ = r;
}

void WidgetTree::clear_clip() {
    clip_active_ = false;
    pending_clip_ = Rect{};
}

Widget* WidgetTree::find(uint64_t id) {
    Slot* s = find_slot(id);
    return s ? &s->w : nullptr;
}

const Widget* WidgetTree::find(uint64_t id) const {
    for (int i = 0; i < used_; ++i) {
        if (slots_[i].alive && slots_[i].w.id == id) return &slots_[i].w;
    }
    return nullptr;
}

WidgetTree::Slot* WidgetTree::find_slot(uint64_t id) {
    for (int i = 0; i < used_; ++i) {
        if (slots_[i].alive && slots_[i].w.id == id) return &slots_[i];
    }
    return nullptr;
}

WidgetTree::Slot* WidgetTree::alloc(uint64_t id) {
    int idx = -1;
    if (free_head_ >= 0) {
        idx = free_head_;
        free_head_ = slots_[idx].next_free;
        slots_[idx].next_free = -1;
    } else if (used_ < kCapacity) {
        idx = used_++;
    } else {
        return nullptr;
    }
    slots_[idx].alive = true;
    slots_[idx].frame = frame_;
    slots_[idx].w = Widget{};     // 全新控件：值清零、文本清空
    slots_[idx].w.id = id;
    ++alive_count_;
    return &slots_[idx];
}

void WidgetTree::update_all() {
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        touch(slots_[i].w.id, slots_[i].w.kind, slots_[i].w.rect, slots_[i].w.label);
    }
}

void WidgetTree::advance_animations(float dt, const Theme& theme) {
    const ThemeAnim& anim = theme_anim();
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        Widget& w = slots_[i].w;
        uint32_t target = 0;
        if (!anim.enabled || !widget_anim_fill(w, theme, &target)) {
            w.anim_from = 0;
            w.anim_to   = 0;
            w.anim_clock = 0.0f;
            continue;
        }
        if (w.anim_to == 0) {
            // 首次进入动画集合：直接落位（不从头过渡——控件是新建出来的，
            // 没有"上一个颜色"可过渡）
            w.anim_from  = target;
            w.anim_to    = target;
            w.anim_clock = 0.0f;
            continue;
        }
        if (w.anim_to != target) {
            // 目标变了（悬停/按下/勾选切换）：从当前插值位置出发重新计时，
            // 快速来回扫鼠标不会跳色
            w.anim_from  = widget_fill_now(w, theme);
            w.anim_to    = target;
            w.anim_clock = 0.0f;
        }
        if (dt > 0.0f) w.anim_clock += dt;
    }
}

void WidgetTree::drop_focus() {
    if (focus_id_ == 0) return;
    // 焦点走的这一刻才算"编辑结束"：改过内容才置 deactivated_after_edit，
    // 只是点进来看看的那次不算。标志只活一帧（touch 每帧清零）。
    if (Widget* w = find(focus_id_)) {
        w->preedit[0] = '\0';
        if (w->edited_while_focused) {
            w->deactivated_after_edit = true;
            w->deactivated_frame = frame_;
            w->edited_while_focused = false;
        }
    }
    focus_id_ = 0;
}

void WidgetTree::focus_widget(uint64_t id) {
    if (id == 0 || focus_id_ == id) return;
    drop_focus();
    Widget* w = find(id);
    if (!w) return;
    focus_id_ = id;
    // 插入点到末尾、空选区：外部请求焦点时用户是要"接着改"，不是要全选。
    w->caret = text_len(*w);
    w->sel_anchor = -1;
    w->edited_while_focused = false;
    w->preedit[0] = '\0';
}

void WidgetTree::end_frame() {
    // 松开即释放拖拽捕获。本帧的 touch 已经读过 clicked（end_frame 在全部
    // touch 之后跑），这里清掉不会吞事件；控件中途消失时也不会留下野 active。
    if (!input_.mouse_down[0]) active_id_ = 0;
    // 点击落在空白处：没有任何控件认领这次按下 → 清键盘焦点
    if (input_.mouse_clicked[0] && press_claimed_ == 0) drop_focus();
    drag_last_x_ = input_.mouse_x;
    if (!input_.mouse_down[0]) drag_owner_ = 0;

    // 输入法组合态只属于"本帧结束时仍持有焦点"的那个输入框。焦点可能在本帧
    // 中途才转移（后提交的输入框抢走焦点），先提交的那个此时已经把预编辑抄进
    // 自己身上了——帧末统一清掉，不然失焦的框会顶着一串组合字。
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        if (slots_[i].w.id != focus_id_) slots_[i].w.preedit[0] = '\0';
    }
}

void WidgetTree::collect_in_submit_order(const Widget** out, int cap, int* count) const {
    int n = 0;
    for (int i = 0; i < used_ && n < cap; ++i) {
        if (slots_[i].alive) out[n++] = &slots_[i].w;
    }
    // 控件数受 kCapacity（512）限制，量级几百，排序代价相对整帧绘制可以忽略；
    // 换来的是"绘制序 = 提交序"这条绘制层依赖的硬约定。
    std::sort(out, out + n, [](const Widget* a, const Widget* b) {
        return a->seq < b->seq;
    });
    if (count) *count = n;
}

void WidgetTree::sweep() {
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        if (slots_[i].frame == frame_) continue;   // 本帧提交过：留着
        slots_[i].alive = false;
        slots_[i].next_free = free_head_;
        free_head_ = i;
        --alive_count_;
    }
    // 归属 id 跟着控件一起失效。sweep 跑在 end_frame（本帧全部提交之后），
    // 所以"没提交"就是"这个控件没了"——面板被关掉、条件 UI 收起来都会这样。
    // 不清的后果不是内存问题而是行为问题：focus_id_ 挂着野 id 时
    // wants_keyboard() 恒真，宿主从此不再派发快捷键（gizmo 的 W/R/F、
    // Ctrl+S 全灭），要等用户随手点一下空白处才自愈。
    if (focus_id_   != 0 && find_slot(focus_id_)   == nullptr) focus_id_   = 0;
    // 注：这里直接清而不是 drop_focus() —— 控件已经不在表里了，没有对象可以
    // 记 deactivated_after_edit，宿主那一帧也拿不到它的值。
    if (active_id_  != 0 && find_slot(active_id_)  == nullptr) active_id_  = 0;
    if (drag_owner_ != 0 && find_slot(drag_owner_) == nullptr) drag_owner_ = 0;
    // 拖放源消失 = 这次拖放作废（否则松手时找不到交付目标）
    if (drag_source_ != 0 && find_slot(drag_source_) == nullptr) end_drag();
}

void WidgetTree::begin_drag(uint64_t source_id, const char* type, const char* text) {
    drag_active_ = true;
    drag_source_ = source_id;
    std::snprintf(drag_type_, sizeof(drag_type_), "%s", type ? type : "");
    std::snprintf(drag_text_, sizeof(drag_text_), "%s", text ? text : "");
}

void WidgetTree::end_drag() {
    drag_active_ = false;
    drag_source_ = 0;
    drag_type_[0] = '\0';
    drag_text_[0] = '\0';
}

void WidgetTree::draw(UiDrawList& dl, const Theme& theme, const UiTextMetrics& metrics) const {
    const UiDrawContext ctx{focus_id_, caret_visible_};
    // 两遍：窗口层在前、弹层在后。弹层的提交顺序可能与窗口交叉（宿主在
    // 窗口内容中间弹层），靠 layer 分流比靠调用顺序可靠。
    for (int layer = 0; layer <= 1; ++layer) {
        for (int i = 0; i < used_; ++i) {
            if (!slots_[i].alive) continue;
            const Widget& w = slots_[i].w;
            if (static_cast<int>(w.layer) != layer) continue;
            ui_draw_widget(w, theme, metrics, ctx, dl);
        }
    }
}

void WidgetTree::clear() {
    for (int i = 0; i < used_; ++i) {
        slots_[i] = Slot{};
    }
    used_ = 0;
    free_head_ = -1;
    alive_count_ = 0;
    active_id_ = 0;
    focus_id_ = 0;
    press_claimed_ = 0;
    drag_owner_ = 0;
    drag_last_x_ = 0.0f;
    end_drag();
}

} // namespace gryce_ui
