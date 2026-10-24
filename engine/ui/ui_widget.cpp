// engine/ui/ui_widget.cpp
// retained 控件层实现。语义见 ui_widget.h 头部。

#include "ui/ui_widget.h"

#include <cstdio>

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

namespace {

inline bool rect_contains(const Rect& r, float x, float y) {
    return x >= r.x && x <= r.right() && y >= r.y && y <= r.bottom();
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

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

// draw_rect 只填不描边：边框 = 四条细矩形（主题 BORDER_W 决定粗细）
void draw_frame_border(UiDrawList& dl, const Rect& r, uint32_t color, float w) {
    if (w <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    dl.add_rect(r.x, r.y, r.w, w, color);
    dl.add_rect(r.x, r.bottom() - w, r.w, w, color);
    dl.add_rect(r.x, r.y, w, r.h, color);
    dl.add_rect(r.right() - w, r.y, w, r.h, color);
}

uint32_t theme_color(const Theme& t, int token) {
    return (token >= 0 && token < THEME_COLOR_COUNT) ? t.colors[token] : 0xFFFFFFFFu;
}

float theme_metric(const Theme& t, int token, float fallback) {
    return (token >= 0 && token < THEME_METRIC_COUNT) ? t.metrics[token] : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// ui_draw_widget — 控件绘制（匿名空间外的公共入口，见 ui_widget.h）
// ---------------------------------------------------------------------------

void ui_draw_widget(const Widget& w, const Theme& theme, const UiTextMetrics& metrics,
                    uint64_t focus_id, UiDrawList& dl) {
    const Rect&  r  = w.rect;
    const float  fs = kWidgetFontSize;
    const float  lh = metrics.line_height(fs);
    const float  ty = r.y + (r.h - lh) * 0.5f;   // 行盒在控件里垂直居中
    const float  bw = theme_metric(theme, THEME_METRIC_BORDER_W, 1.0f);
    const uint32_t text_col = w.text_overridden
                            ? w.text_color
                            : theme_color(theme, w.enabled ? THEME_COLOR_TEXT : THEME_COLOR_TEXT_DIM);

    switch (w.kind) {
        case WidgetKind::Label:
            dl.add_text(r.x + 1.0f, ty, w.label, fs, text_col);
            break;

        case WidgetKind::Button: {
            const uint32_t bg = w.bg_overridden ? w.bg_color
                              : !w.enabled  ? theme_color(theme, THEME_COLOR_BUTTON)
                              : w.pressed  ? theme_color(theme, THEME_COLOR_BUTTON_ACTIVE)
                              : w.hovered  ? theme_color(theme, THEME_COLOR_BUTTON_HOVER)
                                           : theme_color(theme, THEME_COLOR_BUTTON);
            dl.add_rect(r.x, r.y, r.w, r.h, bg);
            draw_frame_border(dl, r, theme_color(theme, THEME_COLOR_BORDER), bw);
            const float tw = metrics.width(w.label, fs);
            dl.add_text(r.x + (r.w - tw) * 0.5f, ty, w.label, fs, text_col);
            break;
        }

        case WidgetKind::Panel:
            dl.add_rect(r.x, r.y, r.w, r.h, theme_color(theme, THEME_COLOR_WINDOW_BG));
            draw_frame_border(dl, r, theme_color(theme, THEME_COLOR_BORDER), bw);
            break;

        case WidgetKind::Separator:
            dl.add_rect(r.x, r.y, r.w, 1.0f, theme_color(theme, THEME_COLOR_SEPARATOR));
            break;

        case WidgetKind::Progress: {
            dl.add_rect(r.x, r.y, r.w, r.h, theme_color(theme, THEME_COLOR_SCROLLBAR_BG));
            draw_frame_border(dl, r, theme_color(theme, THEME_COLOR_BORDER), bw);
            const float frac = clampf(w.value, 0.0f, 1.0f);
            if (frac > 0.0f && r.w > bw * 2.0f && r.h > bw * 2.0f) {
                dl.add_rect(r.x + bw, r.y + bw, (r.w - bw * 2.0f) * frac, r.h - bw * 2.0f,
                            theme_color(theme, THEME_COLOR_ACCENT));
            }
            if (w.label[0]) {
                const float tw = metrics.width(w.label, fs);
                dl.add_text(r.x + (r.w - tw) * 0.5f, ty, w.label, fs,
                            theme_color(theme, THEME_COLOR_TEXT));
            }
            break;
        }

        case WidgetKind::Checkbox: {
            const float side = (r.h < 16.0f) ? r.h : 16.0f;
            const float bx = r.x + 2.0f;
            const float by = r.y + (r.h - side) * 0.5f;
            const Rect   box{bx, by, side, side};
            dl.add_rect(box.x, box.y, box.w, box.h, theme_color(theme, THEME_COLOR_BUTTON));
            draw_frame_border(dl, box, theme_color(theme, THEME_COLOR_BORDER), bw);
            if (w.checked) {
                const float inset = 3.0f;
                if (side > inset * 2.0f) {
                    dl.add_rect(box.x + inset, box.y + inset, side - inset * 2.0f,
                                side - inset * 2.0f, theme_color(theme, THEME_COLOR_CHECK_MARK));
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
            dl.add_rect(x0, track_y, x1 - x0, track_h,
                        theme_color(theme, THEME_COLOR_SCROLLBAR_BG));
            if (frac > 0.0f) {
                dl.add_rect(x0, track_y, (x1 - x0) * frac, track_h,
                            theme_color(theme, THEME_COLOR_ACCENT));
            }
            const float gh = (r.h < 14.0f) ? r.h : 14.0f;
            const float gw = gh * 0.7f;
            const float gx = clampf(x0 + (x1 - x0) * frac - gw * 0.5f, r.x, r.right() - gw);
            dl.add_rect(gx, r.y + (r.h - gh) * 0.5f, gw, gh,
                        theme_color(theme, w.pressed ? THEME_COLOR_SLIDER_GRAB_ACTIVE
                                                     : THEME_COLOR_SLIDER_GRAB));
            break;
        }

        case WidgetKind::TextInput: {
            dl.add_rect(r.x, r.y, r.w, r.h, theme_color(theme, THEME_COLOR_BUTTON));
            draw_frame_border(dl, r,
                              theme_color(theme, (focus_id == w.id) ? THEME_COLOR_ACCENT
                                                                    : THEME_COLOR_BORDER),
                              bw);
            // 长文本不许淌出框外：裁剪 + 只画框内那一段
            dl.push_clip(r.x, r.y, r.w, r.h);
            dl.add_text(r.x + 4.0f, ty, w.text, fs, theme_color(theme, THEME_COLOR_TEXT));
            dl.pop_clip();
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// WidgetTree
// ---------------------------------------------------------------------------

void WidgetTree::begin_frame(const UiInputFrame& input) {
    input_ = input;
    ++frame_;
    press_claimed_ = 0;
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
    std::memset(w.label, 0, sizeof(w.label));
    if (label) std::strncpy(w.label, label, sizeof(w.label) - 1);
    s->frame = frame_;
    return &w;
}

Widget* WidgetTree::touch(uint64_t id, WidgetKind kind, const Rect& rect, const char* label) {
    Widget* w = ensure(id, kind, rect, label);
    if (!w) return nullptr;

    // 每帧标志先清零。Label/Panel/Progress/Separator 不参与命中测试：面板
    // 背景一类的大矩形因此不会抢走覆盖在它上面的按钮。
    w->hovered = false;
    w->pressed = false;
    w->clicked = false;
    if (!w->enabled || kind == WidgetKind::Label || kind == WidgetKind::Panel ||
        kind == WidgetKind::Progress || kind == WidgetKind::Separator) {
        return w;
    }

    const bool inside = rect_contains(rect, input_.mouse_x, input_.mouse_y);
    if (inside) w->hovered = true;

    if (inside && input_.mouse_clicked[0]) {
        // 同一帧里后提交的包含鼠标的控件拿到 active（提交序 = 上层序），
        // 重叠时上层拿按下。
        active_id_ = id;
        press_claimed_ = id;
        if (kind == WidgetKind::TextInput) {
            focus_id_ = id;
        } else if (focus_id_ != 0) {
            focus_id_ = 0;   // 点别的控件：焦点转移走
        }
    }
    if (active_id_ == id && input_.mouse_down[0]) w->pressed = true;
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

        case WidgetKind::TextInput: {
            if (focus_id_ != w.id) break;
            for (int i = 0; i < input_.char_count; ++i) {
                const uint32_t cp = input_.chars[i];
                if (cp < 0x20u || cp == 0x7Fu) continue;   // 控制字符不进输入框
                utf8_append(w.text, sizeof(w.text), cp);
            }
            if (input_.backspace) utf8_pop(w.text);
            break;
        }

        default:
            break;
    }
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

void WidgetTree::end_frame() {
    // 松开即释放拖拽捕获。本帧的 touch 已经读过 clicked（end_frame 在全部
    // touch 之后跑），这里清掉不会吞事件；控件中途消失时也不会留下野 active。
    if (!input_.mouse_down[0]) active_id_ = 0;
    // 点击落在空白处：没有任何控件认领这次按下 → 清键盘焦点
    if (input_.mouse_clicked[0] && press_claimed_ == 0) focus_id_ = 0;
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

void WidgetTree::draw(UiDrawList& dl, const Theme& theme, const UiTextMetrics& metrics) const {
    for (int i = 0; i < used_; ++i) {
        if (!slots_[i].alive) continue;
        ui_draw_widget(slots_[i].w, theme, metrics, focus_id_, dl);
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
}

} // namespace gryce_ui
