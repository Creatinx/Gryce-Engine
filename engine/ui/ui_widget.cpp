// engine/ui/ui_widget.cpp
// retained 控件层实现。语义见 ui_widget.h 头部。

#include "ui/ui_widget.h"

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
            const uint32_t base = on_fill(theme_color(t, THEME_COLOR_ACCENT), t);
            *out = w.pressed ? mix_rgba(base, theme_color(t, THEME_COLOR_ACCENT), 0.30f)
                             : base;
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

} // namespace

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
    // 糊成一团灰晕，要么像贴纸。
    const uint32_t shadow_rgb  = theme_color(theme, THEME_COLOR_BORDER_SHADOW) & 0xFFFFFF00u;
    const uint32_t shadow_near = shadow_rgb | 0x78u;
    const uint32_t shadow_far  = shadow_rgb | 0x40u;
    // 控件日常描边：比 BORDER 更贴近控件底色，只勾形状不画线框；悬停/聚焦换 ACCENT
    const uint32_t edge_soft = theme_color(theme, THEME_COLOR_CONTROL_BORDER);
    const uint32_t accent    = theme_color(theme, THEME_COLOR_ACCENT);

    switch (w.kind) {
        case WidgetKind::Label:
            dl.add_text(r.x + 1.0f, ty, w.label, fs, text_col);
            break;

        case WidgetKind::Button: {
            const uint32_t bg = w.bg_overridden ? w.bg_color : widget_fill_now(w, theme);
            // Fluent 是扁平填充：渐变只留 2%~4% 的极微层次给"可按"的方向感，
            // 按下时整体沉下去（顶亮面收掉、底暗面加深）。
            const uint32_t b_edge = !w.enabled ? edge_soft
                                  : w.pressed  ? theme_color(theme, THEME_COLOR_ACCENT_ACTIVE)
                                  : w.hovered  ? accent
                                               : edge_soft;
            draw_round_frame_gradient(dl, r, r_frame,
                                      shade(bg, w.pressed ? 0.01f : 0.04f),
                                      shade(bg, w.pressed ? -0.05f : -0.02f),
                                      b_edge, bw);
            const float tw = metrics.width(w.label, fs);
            dl.add_text(r.x + (r.w - tw) * 0.5f, ty, w.label, fs, text_col);
            break;
        }

        case WidgetKind::Panel: {
            // 弹层（layer 1）用弹层圆角、窗口用窗口圆角：两者的视觉权重不同
            const float rad = (w.layer > 0) ? r_popup : r_window;
            if (w.shadow) {
                dl.add_shadow(r.x, r.y, r.w, r.h, rad, 8.0f, shadow_near);
                dl.add_shadow(r.x, r.y, r.w, r.h, rad, 24.0f, shadow_far);
            }
            if (w.bg_overridden) {
                draw_round_frame(dl, r, rad, w.bg_color,
                                 theme_color(theme, THEME_COLOR_BORDER), bw);
            } else {
                const uint32_t base = theme_color(theme, THEME_COLOR_WINDOW_BG);
                // Fluent 的窗口是平的：只留 2% 的上下层次，几乎察觉不到
                draw_round_frame_gradient(dl, r, rad, shade(base, 0.02f),
                                          shade(base, -0.01f),
                                          mix_rgba(theme_color(theme, THEME_COLOR_BORDER),
                                                   base, 0.20f), bw);
            }
            break;
        }

        case WidgetKind::Separator:
            // 分隔线压到背景附近：满亮度的 BORDER 会在深色面板上画出一道白线
            dl.add_rect(r.x, r.y, r.w, 1.0f,
                        mix_rgba(theme_color(theme, THEME_COLOR_SEPARATOR),
                                 theme_color(theme, THEME_COLOR_WINDOW_BG), 0.45f));
            break;

        case WidgetKind::Progress: {
            // 轨道要"凹"但不能是黑洞：比窗口底色再压一档，描边取自轨道自身
            const uint32_t track =
                mix_rgba(theme_color(theme, THEME_COLOR_SCROLLBAR_BG),
                         theme_color(theme, THEME_COLOR_WINDOW_BG), 0.30f);
            draw_round_frame(dl, r, r_frame, track,
                             mix_rgba(track, theme_color(theme, THEME_COLOR_BORDER), 0.35f), bw);
            const float frac = clampf(w.value, 0.0f, 1.0f);
            if (frac > 0.0f && r.w > bw * 2.0f && r.h > bw * 2.0f) {
                // 内缩一个边框，填充条跟着轨道一起圆（半径要减去边框宽度，
                // 否则填充条的圆角会被边框盖住一半，看起来是方的）；高度小于
                // 两倍半径时按半高走胶囊，否则短进度条会变成小方块。
                float inner_r = clamp_radius(r_frame - bw, r);
                const float fill_h = r.h - bw * 2.0f;
                if (inner_r > fill_h * 0.5f) inner_r = fill_h * 0.5f;
                const uint32_t a = theme_color(theme, THEME_COLOR_ACCENT);
                dl.add_round_rect(r.x + bw, r.y + bw, (r.w - bw * 2.0f) * frac,
                                  fill_h, inner_r, a);
            }
            if (w.label[0]) {
                const float tw = metrics.width(w.label, fs);
                // 标签压在填充条上：用与填充色反差最大的那个主题色
                dl.add_text(r.x + (r.w - tw) * 0.5f, ty, w.label, fs,
                            on_fill(accent, theme));
            }
            break;
        }

        case WidgetKind::Checkbox: {
            const float side = (r.h < 16.0f) ? r.h : 16.0f;
            const float bx = r.x + 2.0f;
            const float by = r.y + (r.h - side) * 0.5f;
            const Rect   box{bx, by, side, side};
            const uint32_t box_bg = widget_fill_now(w, theme);
            const uint32_t box_edge = w.checked ? shade(accent, 0.30f)
                                    : w.hovered ? accent
                                                : edge_soft;
            draw_round_frame_gradient(dl, box, r_grab, shade(box_bg, 0.04f),
                                      shade(box_bg, -0.02f), box_edge, bw);
            if (w.checked) {
                // 没有勾线路径可画：用一块与底色反差最大的内缩圆角块当"已选"。
                // 块不能给大——16px 的方框里塞 10px 白方块像贴了张纸。
                const float inset = 5.0f;
                if (side > inset * 2.0f) {
                    const float inner = side - inset * 2.0f;
                    dl.add_round_rect(box.x + inset, box.y + inset, inner, inner,
                                      inner * 0.30f, on_fill(box_bg, theme));
                }
            }
            dl.add_text(bx + side + 6.0f, ty, w.label, fs,
                        theme_color(theme, w.enabled ? THEME_COLOR_TEXT : THEME_COLOR_TEXT_DIM));
            break;
        }

        case WidgetKind::Slider: {
            const float lw = metrics.width(w.label, fs);
            dl.add_text(r.x + 1.0f, ty, w.label, fs, theme_color(theme, THEME_COLOR_TEXT));
            float x0 = r.x + lw + 8.0f;
            const float x1 = r.right() - 2.0f;
            if (x1 - x0 < 24.0f) x0 = r.x + 2.0f;   // 标签太宽：轨道让位
            const float span = (w.max_value > w.min_value) ? (w.max_value - w.min_value) : 1.0f;
            const float frac = clampf((w.value - w.min_value) / span, 0.0f, 1.0f);
            const float track_h = 4.0f;
            const float track_y = r.y + r.h * 0.5f - track_h * 0.5f;
            const float track_r = track_h * 0.5f;
            const uint32_t groove = mix_rgba(theme_color(theme, THEME_COLOR_SCROLLBAR_BG),
                                             theme_color(theme, THEME_COLOR_WINDOW_BG), 0.30f);
            dl.add_round_rect(x0, track_y, x1 - x0, track_h, track_r, groove);
            if (frac > 0.0f) {
                const uint32_t a = theme_color(theme, THEME_COLOR_ACCENT);
                dl.add_round_rect(x0, track_y, (x1 - x0) * frac, track_h, track_r,
                                  shade(a, 0.06f));
            }
            const float gh = (r.h < 15.0f) ? r.h : 15.0f;
            const float gw = gh * 0.72f;
            const float gx = clampf(x0 + (x1 - x0) * frac - gw * 0.5f, r.x, r.right() - gw);
            // 抓柄是胶囊：半径取半宽，缩放后也不会变成小方块。颜色要跟已经填成
            // ACCENT 的轨道拉开——同色抓柄在那个位置等于看不见。
            const Rect grab{gx, r.y + (r.h - gh) * 0.5f, gw, gh};
            const uint32_t knob = widget_fill_now(w, theme);
            draw_round_frame(dl, grab, gw * 0.5f, knob,
                             mix_rgba(knob, accent, 0.45f), bw);
            break;
        }

        case WidgetKind::DragFloat: {
            // 与 ImGui 的形态一致：一条可拖的"值框"，左标签、右数值。
            const uint32_t bg = widget_fill_now(w, theme);
            draw_round_frame_gradient(dl, r, r_frame,
                                      shade(bg, w.pressed ? 0.01f : 0.04f),
                                      shade(bg, w.pressed ? -0.05f : -0.02f),
                                      (w.hovered || w.pressed) ? accent : edge_soft, bw);
            if (w.label[0]) {
                dl.add_text(r.x + 6.0f, ty, w.label, fs, theme_color(theme, THEME_COLOR_TEXT));
            }
            char val[32];
            std::snprintf(val, sizeof(val), "%.3f", w.value);
            const float vw = metrics.width(val, fs);
            dl.add_text(r.right() - vw - 6.0f, ty, val, fs,
                        theme_color(theme, THEME_COLOR_TEXT));
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
            draw_round_frame_gradient(dl, r, r_frame, shade(bg, 0.03f),
                                      shade(bg, -0.01f), 0u, 0.0f);
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
            if (w.shadow) dl.add_shadow(r.x, r.y, r.w, r.h, r_child, 14.0f, shadow_far);
            {
                const uint32_t base = w.bg_overridden
                                    ? w.bg_color
                                    : theme_color(theme, THEME_COLOR_CHILD_BG);
                draw_round_frame_gradient(dl, r, r_child, shade(base, 0.02f),
                                          shade(base, -0.01f),
                                          mix_rgba(theme_color(theme, THEME_COLOR_BORDER), base, 0.45f),
                                          bw);
            }
            break;

        case WidgetKind::TitleBar: {
            const uint32_t bg = !w.enabled ? theme_color(theme, THEME_COLOR_TITLE_BG_COLLAPSED)
                              : w.pressed  ? theme_color(theme, THEME_COLOR_TITLE_BG_ACTIVE)
                                           : theme_color(theme, THEME_COLOR_TITLE_BG);
            // 标题栏只有上面两个角跟着窗口圆：下面要接内容区，圆了就露缝。
            // 用逐角半径的渐变，一次搞定"圆角 + 纵向层次"，不再叠第二个矩形
            //（叠加会在圆角外露出一圈方块边）。
            const float tradii[4] = {r_window, r_window, 0.0f, 0.0f};
            dl.add_round_gradient4(r.x, r.y, r.w, r.h, tradii, shade(bg, 0.05f),
                                   shade(bg, -0.02f));
            // 标题栏自己再描一圈边。窗口底板的描边先画、被标题栏盖住，不补这一
            // 条时窗口上沿是没有边框的，而且圆角处会露出一像素的底板色（看着
            // 像缺口）。下沿那条边同时充当"标题栏 / 内容区"的分隔线——标题栏与
            // 内容区同色系，没有这条线根本看不出哪一块能拖。
            dl.add_round_border4(r.x, r.y, r.w, r.h, tradii, bw,
                                 mix_rgba(theme_color(theme, THEME_COLOR_BORDER), bg, 0.25f));
            draw_fold_marker(dl, r.x + 5.0f, ty, w.open, theme, metrics, fs);
            dl.add_text(r.x + 12.0f + fs * 0.75f, ty, w.label, fs,
                        theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::Tooltip: {
            if (w.shadow) {
                dl.add_shadow(r.x, r.y, r.w, r.h, r_popup, 7.0f, shadow_near);
                dl.add_shadow(r.x, r.y, r.w, r.h, r_popup, 20.0f, shadow_far);
            }
            const uint32_t tbg = theme_color(theme, THEME_COLOR_POPUP_BG);
            draw_round_frame_gradient(dl, r, r_popup, shade(tbg, 0.03f),
                                      shade(tbg, -0.01f),
                                      theme_color(theme, THEME_COLOR_BORDER), bw);
            dl.add_text(r.x + 6.0f, ty, w.label, fs, theme_color(theme, THEME_COLOR_TEXT));
            break;
        }

        case WidgetKind::TextInput: {
            // 输入框要看起来"凹进去"：底色比按钮更沉，渐变反着走（顶暗底亮）。
            // 平涂的输入框与按钮长得一模一样，用户看不出哪里能打字。
            const uint32_t base = mix_rgba(theme_color(theme, THEME_COLOR_BUTTON),
                                           theme_color(theme, THEME_COLOR_CHILD_BG), 0.55f);
            draw_round_frame_gradient(dl, r, r_frame, shade(base, -0.03f),
                                      shade(base, 0.02f),
                                      focused ? accent : edge_soft,
                                      focused ? (bw + 0.5f) : bw);
            // 长文本不许淌出框外：裁剪 + 只画框内那一段
            dl.push_clip(r.x, r.y, r.w, r.h);
            const float tx = r.x + 4.0f - w.scroll_x;
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

    // 每帧标志先清零。Label/Panel/Progress/Separator/装饰件不参与命中测试：
    // 面板背景一类的大矩形因此不会抢走覆盖在它上面的按钮。
    w->hovered = false;
    w->pressed = false;
    w->clicked = false;
    if (!w->enabled || kind == WidgetKind::Label || kind == WidgetKind::Panel ||
        kind == WidgetKind::Progress || kind == WidgetKind::Separator ||
        kind == WidgetKind::Tooltip || kind == WidgetKind::ClipBegin ||
        kind == WidgetKind::ClipEnd || kind == WidgetKind::Bar ||
        kind == WidgetKind::Frame) {
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
            focus_id_ = 0;   // 点别的控件：焦点转移走
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

        case WidgetKind::TextInput:
            edit_text(w);
            break;

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
                w.sel_anchor = -1;
                w.preedit[0] = '\0';
                focus_id_ = 0;
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

void WidgetTree::end_frame() {
    // 松开即释放拖拽捕获。本帧的 touch 已经读过 clicked（end_frame 在全部
    // touch 之后跑），这里清掉不会吞事件；控件中途消失时也不会留下野 active。
    if (!input_.mouse_down[0]) active_id_ = 0;
    // 点击落在空白处：没有任何控件认领这次按下 → 清键盘焦点
    if (input_.mouse_clicked[0] && press_claimed_ == 0) focus_id_ = 0;
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

void WidgetTree::sweep() {
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        if (slots_[i].frame == frame_) continue;   // 本帧提交过：留着
        slots_[i].alive = false;
        slots_[i].next_free = free_head_;
        free_head_ = i;
        --alive_count_;
    }
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
