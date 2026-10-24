// engine/ui/ui_builtin.cpp
// 内置即时模式层实现。语义与边界见 ui_builtin.h 头部。

#include "ui/ui_builtin.h"

#include <cstdio>
#include <cstring>

#include "ui/ui_id.h"

namespace gryce_ui {

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------

void BuiltinUi::begin_frame(const UiInputFrame& input, const Theme& theme,
                            const UiTextMetrics& metrics) {
    tree_.begin_frame(input);
    theme_   = &theme;
    metrics_ = &metrics;
    depth_   = 0;
    item_seq_ = 0;
    last_rect_     = Rect{};
    last_hovered_  = false;
    last_pressed_  = false;
    mouse_in_window_ = false;
}

void BuiltinUi::end_frame() {
    tree_.end_frame();
    tree_.sweep();
}

void BuiltinUi::draw(UiDrawList& dl) {
    if (!theme_ || !metrics_) return;

    // 单遍按提交序走：遇 Panel（窗口 chrome）切换裁剪域。chrome 本身不裁
    // （它就是窗框），内容裁在窗口矩形里。窗口不交叉开时这等于"每个面板
    // 的内容裁在自己家里"，且保持精确的提交序。
    bool clip_pushed = false;
    tree_.for_each_alive([&](const Widget& w) {
        if (w.kind == WidgetKind::Panel) {
            if (clip_pushed) {
                dl.pop_clip();
                clip_pushed = false;
            }
            ui_draw_widget(w, *theme_, *metrics_, tree_.focus_id(), dl);
            // 内容裁剪域 = 窗口矩形内缩 PAD。退化（窗口比内边距还小）时域为
            // 空：照样 push——drawlist 对空域的执行是"绘制命令全裁、裁剪命令
            // 照记"，正是要的行为；跳过 push 反而会让内容淌出面板。
            const Rect c{w.rect.x + pad_x(), w.rect.y + pad_y(),
                         w.rect.w - pad_x() * 2.0f, w.rect.h - pad_y() * 2.0f};
            dl.push_clip(c.x, c.y, c.w, c.h);
            clip_pushed = true;
            return;
        }
        ui_draw_widget(w, *theme_, *metrics_, tree_.focus_id(), dl);
    });
    if (clip_pushed) dl.pop_clip();
}

// ---------------------------------------------------------------------------
// 窗口栈
// ---------------------------------------------------------------------------

int BuiltinUi::begin_window(const char* name, const Rect& rect) {
    if (depth_ >= kMaxWindows) return 0;
    if (rect.w <= 0.0f || rect.h <= 0.0f) return 0;

    WindowState& win = windows_[depth_];
    win.id    = hash_label(name);
    win.rect  = rect;
    win.cursor_y = rect.y + pad_y();
    win.last_rect = Rect{};
    win.pending_same_line = false;
    win.same_line_spacing = -1.0f;
    ++depth_;

    // "鼠标在 UI 上"的判据：落在任何一个窗口矩形里（内容区或留白都算）。
    // WantCaptureMouse 要的是这个粒度——落进面板就不该把点击交给视口。
    const UiInputFrame& in = tree_.input();
    if (in.mouse_x >= rect.x && in.mouse_x <= rect.right() &&
        in.mouse_y >= rect.y && in.mouse_y <= rect.bottom()) {
        mouse_in_window_ = true;
    }

    // chrome = 底 + 边框，不参与命中测试（点面板空白处要能清键盘焦点）。
    // scope 保持 0：chrome 不属于任何窗口的内容区。
    Widget* w = tree_.touch(win.id, WidgetKind::Panel, rect, name);
    if (w) w->scope = 0;
    return 1;
}

void BuiltinUi::end_window() {
    if (depth_ > 0) --depth_;
}

Rect BuiltinUi::current_window_rect() const {
    return (depth_ > 0) ? windows_[depth_ - 1].rect : Rect{};
}

// ---------------------------------------------------------------------------
// 光标流
// ---------------------------------------------------------------------------

void BuiltinUi::same_line(float spacing) {
    if (depth_ == 0) return;
    WindowState& win = windows_[depth_ - 1];
    win.pending_same_line = true;
    win.same_line_spacing = spacing;
}

void BuiltinUi::spacing(float px) {
    if (depth_ == 0) return;
    // px < 0 = "用主题的行间距"（GUi_Spacing 的语义：空一行）；px == 0 是空操作。
    if (px < 0.0f) {
        windows_[depth_ - 1].cursor_y += item_spacing();
        return;
    }
    if (px == 0.0f) return;
    windows_[depth_ - 1].cursor_y += px;
}

Rect BuiltinUi::place(float w, float h) {
    if (depth_ == 0) return Rect{};
    WindowState& win = windows_[depth_ - 1];
    const float cw = content_width();

    Rect r;
    if (win.pending_same_line) {
        const float sp = (win.same_line_spacing >= 0.0f) ? win.same_line_spacing : item_spacing();
        r = Rect{win.last_rect.right() + sp, win.last_rect.y, w, h};
        win.pending_same_line = false;
        // 行内 item 比行高还高时，把下一行推到它下面（否则会叠住）
        const float line_end = r.bottom() + item_spacing();
        if (line_end > win.cursor_y) win.cursor_y = line_end;
    } else {
        r = Rect{win.rect.x + pad_x(), win.cursor_y, w, h};
        win.cursor_y += h + item_spacing();
    }

    // 右边界按内容区裁；宽度不够就缩到 0（不画也不交互，但光标照走）
    const float right = win.rect.right() - pad_x();
    if (r.x + r.w > right) r.w = right - r.x;
    if (r.w < 0.0f) r.w = 0.0f;
    if (r.w > cw) r.w = cw;

    win.last_rect = r;
    return r;
}

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

namespace {

uint32_t pack_rgba(const float* rgba) {
    if (!rgba) return 0xFF000000u;
    float c[4];
    for (int i = 0; i < 4; ++i) {
        float v = rgba[i];
        c[i] = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    const uint32_t r = static_cast<uint32_t>(c[0] * 255.0f + 0.5f);
    const uint32_t g = static_cast<uint32_t>(c[1] * 255.0f + 0.5f);
    const uint32_t b = static_cast<uint32_t>(c[2] * 255.0f + 0.5f);
    const uint32_t a = static_cast<uint32_t>(c[3] * 255.0f + 0.5f);
    return (r << 24) | (g << 16) | (b << 8) | a;
}

// "Text##id" → "Text"（ImGui 惯例：'##' 之后只参与 id，不显示）。
// **id 仍用完整标签**：同窗口里 "A##1" 与 "A##2" 必须是两个控件。
// "##id" 形态 = 纯 id 控件，显示为空串（按钮只剩内边距）。
void copy_display_label(const char* label, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!label) return;
    const char* sep = std::strstr(label, "##");
    const size_t n  = sep ? static_cast<size_t>(sep - label) : std::strlen(label);
    const size_t m  = (n < cap - 1) ? n : cap - 1;
    std::memcpy(out, label, m);
    out[m] = '\0';
}

} // namespace

// 记录"刚提交的 item"查询状态；w 为空（无窗口/表满）时清空。
void BuiltinUi::finish_impl(Widget* w) {
    if (!w) {
        last_rect_ = Rect{};
        last_hovered_ = false;
        last_pressed_ = false;
        return;
    }
    last_rect_ = w->rect;
    last_hovered_ = w->hovered;
    last_pressed_ = w->pressed;
}

void BuiltinUi::text(const char* s) {
    if (depth_ == 0) { finish_impl(nullptr); return; }
    text_wrapped(s, 0, false);
}

void BuiltinUi::text_colored(uint32_t rgba, const char* s) {
    if (depth_ == 0) { finish_impl(nullptr); return; }
    text_wrapped(s, rgba, true);
}

// 单行 Label：几何 + 着色都在这里，wrap 出来的每一行也走它。
void BuiltinUi::emit_label(const char* s, uint32_t rgba, bool colored) {
    const Rect r = place(content_width(), line_height());
    Widget* w = tree_.touch(item_id(s, true), WidgetKind::Label, r, s);
    if (w) {
        w->scope = windows_[depth_ - 1].id;
        if (colored) {
            w->text_overridden = true;
            w->text_color = rgba;
        }
    }
    finish_impl(w);
}

// 自动换行：ImGui 的 TextUnformatted 默认按可用宽度折行，内置轨必须自己折
// ——不然长句子被面板右边界裁掉，用户看到的是半截话。
//
// 规则（够用即可，不做排版算法）：优先在空格后断行；一个词比整行还宽时按
// UTF-8 码点边界硬断；行容量受 kWidgetLabelCap 限制（与 Label 控件同口径）。
void BuiltinUi::text_wrapped(const char* s, uint32_t rgba, bool colored) {
    if (!s) s = "";
    const float avail = content_width();
    const float fs = kWidgetFontSize;

    // 够宽就一行交出去（绝大多数情况走这里，包括"没有度量器"）
    if (!metrics_ || avail <= 0.0f || metrics_->width(s, fs) <= avail) {
        emit_label(s, rgba, colored);
        return;
    }

    char line[kWidgetLabelCap];
    int  len = 0;
    int  last_break = -1;   // line 里最后一个可断位置（空格之后）
    line[0] = '\0';

    const char* p = s;
    while (*p) {
        // 取一个 UTF-8 码点（不做合法性校验：非法字节按单字节走）
        int n = 1;
        const unsigned char c = static_cast<unsigned char>(*p);
        if      ((c & 0xE0u) == 0xC0u) n = 2;
        else if ((c & 0xF0u) == 0xE0u) n = 3;
        else if ((c & 0xF8u) == 0xF0u) n = 4;
        for (int i = 1; i < n; ++i) {
            if (!p[i]) { n = i; break; }
        }

        // 行容量满：先落地，再从头开始
        if (len + n >= kWidgetLabelCap) {
            emit_label(line, rgba, colored);
            len = 0;
            last_break = -1;
            line[0] = '\0';
            continue;
        }

        std::memcpy(line + len, p, static_cast<size_t>(n));
        len += n;
        line[len] = '\0';
        if (line[len - 1] == ' ') last_break = len;
        p += n;

        if (metrics_->width(line, fs) > avail) {
            if (last_break > 0) {
                // 退到最后一个空格：尾部搬到下一行
                char carry[kWidgetLabelCap];
                std::snprintf(carry, sizeof(carry), "%s", line + last_break);
                int cut = last_break;
                while (cut > 0 && line[cut - 1] == ' ') --cut;
                line[cut] = '\0';
                if (cut > 0) emit_label(line, rgba, colored);
                len = static_cast<int>(std::strlen(carry));
                std::memcpy(line, carry, static_cast<size_t>(len) + 1);
                last_break = -1;
            } else {
                // 没有断点：硬断，把这个码点留给下一行
                len -= n;
                line[len] = '\0';
                if (len > 0) emit_label(line, rgba, colored);
                std::memcpy(line, p - n, static_cast<size_t>(n));
                len = n;
                line[len] = '\0';
            }
        }
    }
    if (len > 0) emit_label(line, rgba, colored);
}

void BuiltinUi::label_text(const char* label, const char* text) {
    if (depth_ == 0) { finish_impl(nullptr); return; }
    char buf[kWidgetLabelCap];
    std::snprintf(buf, sizeof(buf), "%s: %s", label ? label : "", text ? text : "");
    const Rect r = place(content_width(), frame_height());
    Widget* w = tree_.touch(item_id(label, true), WidgetKind::Label, r, buf);
    if (w) w->scope = windows_[depth_ - 1].id;
    finish_impl(w);
}

void BuiltinUi::separator() {
    if (depth_ == 0) { finish_impl(nullptr); return; }
    const Rect r = place(content_width(), 1.0f);
    Widget* w = tree_.touch(item_id("##separator", true), WidgetKind::Separator, r, nullptr);
    if (w) w->scope = windows_[depth_ - 1].id;
    finish_impl(w);
}

int BuiltinUi::button(const char* label) {
    if (depth_ == 0) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
    const float w = tw + pad_x() * 2.0f;
    const Rect r = place(w, frame_height());
    Widget* wd = tree_.touch(item_id(label, false), WidgetKind::Button, r, shown);
    if (wd) wd->scope = windows_[depth_ - 1].id;
    finish_impl(wd);
    return (wd && wd->clicked) ? 1 : 0;
}

int BuiltinUi::button_size(const char* label, float w, float h) {
    if (depth_ == 0) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    if (w <= 0.0f) {
        const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
        w = tw + pad_x() * 2.0f;
    }
    if (h <= 0.0f) h = frame_height();
    const Rect r = place(w, h);
    Widget* wd = tree_.touch(item_id(label, false), WidgetKind::Button, r, shown);
    if (wd) wd->scope = windows_[depth_ - 1].id;
    finish_impl(wd);
    return (wd && wd->clicked) ? 1 : 0;
}

int BuiltinUi::checkbox(const char* label, int* inout_value) {
    if (depth_ == 0) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(content_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const bool was = (inout_value != nullptr && *inout_value != 0);

    // 先 ensure + 同步调用方的值（权威在调用方），再 touch 跑交互
    Widget* w = tree_.ensure(id, WidgetKind::Checkbox, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    w->checked = was;
    w = tree_.touch(id, WidgetKind::Checkbox, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    finish_impl(w);

    if (w->checked != was) {
        if (inout_value) *inout_value = w->checked ? 1 : 0;
        return 1;
    }
    return 0;
}

int BuiltinUi::slider_float(const char* label, float* inout_value, float vmin, float vmax) {
    if (depth_ == 0) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(content_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const float was = inout_value ? *inout_value : 0.0f;

    Widget* w = tree_.ensure(id, WidgetKind::Slider, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    w->min_value = vmin;
    w->max_value = (vmax > vmin) ? vmax : vmin + 1.0f;
    w->value = was;
    w = tree_.touch(id, WidgetKind::Slider, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    finish_impl(w);

    if (w->value != was) {
        if (inout_value) *inout_value = w->value;
        return 1;
    }
    return 0;
}

int BuiltinUi::input_text(const char* label, char* buf, int buf_size) {
    if (depth_ == 0 || !buf || buf_size <= 1) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(content_width(), frame_height());
    const uint64_t id = item_id(label, false);

    Widget* w = tree_.ensure(id, WidgetKind::TextInput, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    // 调用方缓冲区 → 控件（超出控件容量的部分截断；焦点/编辑态在树里）
    std::strncpy(w->text, buf, sizeof(w->text) - 1);
    w->text[sizeof(w->text) - 1] = '\0';

    char before[kWidgetTextCap];
    std::snprintf(before, sizeof(before), "%s", w->text);

    w = tree_.touch(id, WidgetKind::TextInput, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    w->scope = windows_[depth_ - 1].id;
    finish_impl(w);

    // 读回（同样按调用方容量截断）
    std::strncpy(buf, w->text, static_cast<size_t>(buf_size) - 1);
    buf[buf_size - 1] = '\0';
    return (std::strcmp(before, w->text) != 0) ? 1 : 0;
}

void BuiltinUi::progress_bar(float fraction, const char* overlay) {
    if (depth_ == 0) { finish_impl(nullptr); return; }
    const Rect r = place(content_width(), frame_height());
    Widget* w = tree_.touch(item_id(overlay ? overlay : "##progress", true),
                            WidgetKind::Progress, r, overlay);
    if (w) {
        w->scope = windows_[depth_ - 1].id;
        w->value = fraction;
    }
    finish_impl(w);
}

int BuiltinUi::color_button(const char* label, const float rgba[4]) {
    if (depth_ == 0) { finish_impl(nullptr); return 0; }
    // 与 ImGui 一致：ColorButton 的标签只当 id 用，不出现在界面上（块内就是
    // 色块本身）。所以这里给控件空标签，但 id 仍取完整 label。
    const float side = frame_height();
    const Rect r = place(side, side);
    Widget* w = tree_.touch(item_id(label, false), WidgetKind::Button, r, "");
    if (w) {
        w->scope = windows_[depth_ - 1].id;
        w->bg_overridden = true;
        w->bg_color = pack_rgba(rgba);
    }
    finish_impl(w);
    return (w && w->clicked) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 内部小件
// ---------------------------------------------------------------------------

uint64_t BuiltinUi::item_id(const char* label, bool stateless) {
    const uint64_t win = (depth_ > 0) ? windows_[depth_ - 1].id : 0;
    if (!stateless) return hash_label(label, win);
    // 无状态控件混入帧内序号：同窗口同标签的两个 slot 必须是两个控件
    const uint64_t seq = static_cast<uint64_t>(item_seq_++) * 0x9E3779B97F4A7C15ull;
    return hash_label(label, win ^ seq);
}

float BuiltinUi::pad_x() const {
    return (theme_ && THEME_METRIC_PAD_X < THEME_METRIC_COUNT) ? theme_->metrics[THEME_METRIC_PAD_X] : 8.0f;
}

float BuiltinUi::pad_y() const {
    return (theme_ && THEME_METRIC_PAD_Y < THEME_METRIC_COUNT) ? theme_->metrics[THEME_METRIC_PAD_Y] : 4.0f;
}

float BuiltinUi::item_spacing() const {
    return (theme_ && THEME_METRIC_ITEM_SPACING < THEME_METRIC_COUNT)
               ? theme_->metrics[THEME_METRIC_ITEM_SPACING] : 4.0f;
}

float BuiltinUi::line_height() const {
    return metrics_ ? metrics_->line_height(kWidgetFontSize) : kWidgetFontSize * 1.25f;
}

float BuiltinUi::frame_height() const {
    return line_height() + pad_y() * 2.0f;
}

float BuiltinUi::content_width() const {
    if (depth_ == 0) return 0.0f;
    const Rect& r = windows_[depth_ - 1].rect;
    return (r.w > pad_x() * 2.0f) ? (r.w - pad_x() * 2.0f) : 0.0f;
}

} // namespace gryce_ui
