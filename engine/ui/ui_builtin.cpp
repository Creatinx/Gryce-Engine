// engine/ui/ui_builtin.cpp
// 内置即时模式层实现。语义与边界见 ui_builtin.h 头部。

#include "ui/ui_builtin.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui/ui_id.h"
#include "ui/ui_texture_v_flip.h"

namespace gryce_ui {

namespace {

// 后端纹理采样约定（见 ui_texture_v_flip.h）。默认 false = D3D12 / Vulkan 约定；
// GRender_Init 按后端覆写。内置轨的 image() 读它算 sprite 的 uv。
bool g_texture_v_flip = false;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline bool rect_contains(const Rect& r, float x, float y) {
    return x >= r.x && x <= r.right() && y >= r.y && y <= r.bottom();
}

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

// 拖动判定阈值：小于它算点击/抖动，不算拖放
constexpr float kDragThreshold = 4.0f;
// 工具提示的悬停延时（宿主时钟为 0 时不等，便于单测与截图）
constexpr float kTooltipDelay = 0.35f;

} // namespace

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------

void BuiltinUi::set_scale(float s) {
    scale_ = (s > 0.0f) ? s : 1.0f;
}

void BuiltinUi::set_display_size(float w, float h) {
    display_w_ = (w > 0.0f) ? w : 0.0f;
    display_h_ = (h > 0.0f) ? h : 0.0f;
}

void BuiltinUi::begin_frame(const UiInputFrame& input, const Theme& theme,
                            const UiTextMetrics& metrics) {
    // 帧首清掉一次性请求：宿主若设了宽度却没提交吃宽度的控件，不能漏到下一帧
    next_item_width_ = 0.0f;
    focus_next_ = false;
    // HiDPI：宿主给的是像素坐标，模型层统一在逻辑坐标里算（渲染桥乘回去）
    UiInputFrame in = input;
    if (scale_ != 1.0f) {
        in.mouse_x /= scale_;
        in.mouse_y /= scale_;
    }
    mouse_x_ = in.mouse_x;
    mouse_y_ = in.mouse_y;
    if (in.mouse_clicked[0]) {        press_x_ = in.mouse_x;
        press_y_ = in.mouse_y;
    }

    ++frame_no_;
    dismiss_x_ = -1.0f;
    dismiss_y_ = -1.0f;

    tree_.begin_frame(in, &metrics);
    theme_   = &theme;
    metrics_ = &metrics;
    depth_   = 0;
    item_seq_ = 0;
    last_rect_     = Rect{};
    last_hovered_  = false;
    last_pressed_  = false;
    last_id_       = 0;
    mouse_in_window_ = false;
    mouse_in_popup_  = false;
    popup_begun_     = false;
    menu_begun_      = false;

    // 弹层关闭：Esc 一律关；非模态时点外面也关（模态必须显式关）。
    // 放在 begin_frame（而不是 end_frame）——这样"点外面"的那一帧
    // begin_popup 就已经返回 0，与 ImGui 的 NewFrame 语义一致；
    // 同时天然避开"开弹层的那一帧被自己的点击判成点外面"。
    // 命中域用上一帧量到的矩形（自适应高度取 measured_h）。
    if (popup_.open) {
        const float h = (popup_.h > 0.0f) ? popup_.h
                      : ((popup_.measured_h > 0.0f) ? popup_.measured_h : frame_height() * 2.0f);
        // 宽度同样要取实测值：自适应宽度的菜单一旦比 180 宽，用默认值判
        // "点外面"会把点在自己身上的那一下当成外部点击，菜单一碰就关。
        const float pw = (popup_.w > 0.0f) ? popup_.w
                       : ((popup_.measured_w > 1.0f) ? popup_.measured_w : 180.0f);
        const Rect pr{popup_.x, popup_.y, pw, h};
        const bool esc = in.key_pressed(UiInputFrame::KeyEsc);
        const bool clicked_outside = in.mouse_clicked[0] &&
                                     !rect_contains(pr, in.mouse_x, in.mouse_y);
        if (esc || (!popup_.modal && clicked_outside)) {
            popup_.open = false;
            if (clicked_outside) {
                dismiss_x_ = in.mouse_x;
                dismiss_y_ = in.mouse_y;
            }
        }
    }
}

void BuiltinUi::end_frame() {
    tree_.end_frame();
    tree_.sweep();
}

void BuiltinUi::draw(UiDrawList& dl) {
    if (!theme_ || !metrics_) return;

    // 状态过渡动画：时间源是输入快照的 time（0 = 无时钟，动画原地踏步）。
    // 放在 draw 开头——本帧全部 touch 已落地（hovered/pressed 是最终值），
    // 推进后下面的绘制读到的就是插值后的颜色。
    {
        const float t = tree_.input().time;
        float dt = 0.0f;
        if (prev_anim_time_ > 0.0f && t > prev_anim_time_) dt = t - prev_anim_time_;
        if (t > 0.0f) prev_anim_time_ = t;
        tree_.advance_animations(dt, *theme_);
    }

    const UiDrawContext ctx{tree_.focus_id(), tree_.caret_visible()};

    // 绘制序 = 本帧提交序（**不是**控件池的槽位序）。Panel 推内容裁剪、
    // ClipBegin/ClipEnd 压弹，它们与内容的相对顺序一旦被打乱，内容就会落进
    // 别的面板的裁剪域里被整片裁掉。
    const Widget* order[WidgetTree::kCapacity];
    int           order_count = 0;
    tree_.collect_in_submit_order(order, WidgetTree::kCapacity, &order_count);

    // 两遍：窗口层在前、弹层在后。层内按提交序遍历；遇 Panel（窗口/弹层
    // 背景板）切换裁剪域，遇 ClipBegin/ClipEnd 压/弹子区域裁剪。
    // 用 clip_depth 记账而不是布尔：子区域可能嵌在窗口的裁剪里。
    for (int layer = 0; layer <= 1; ++layer) {
        int clip_depth = 0;
        // 最近一个面板的内容裁剪域：标题栏要用它"临时出界"（见下面的分支）
        Rect panel_clip{};
        for (int oi = 0; oi < order_count; ++oi) {
            const Widget& w = *order[oi];
            if (static_cast<int>(w.layer) != layer) continue;

            if (w.kind == WidgetKind::ClipEnd) {
                if (clip_depth > 0) { dl.pop_clip(); --clip_depth; }
                continue;
            }
            if (w.kind == WidgetKind::Panel) {
                // 上一个面板的内容裁剪到此结束
                while (clip_depth > 0) { dl.pop_clip(); --clip_depth; }
                ui_draw_widget(w, *theme_, *metrics_, ctx, dl);
                // 内容裁剪域 = 面板矩形内缩 PAD。退化（面板比内边距还小）时域
                // 为空：照样 push——drawlist 对空域的执行是"绘制命令全裁、裁剪
                // 命令照记"，正是要的行为；跳过 push 反而会让内容淌出面板。
                const Rect c{w.rect.x + pad_x(), w.rect.y + pad_y(),
                             w.rect.w - pad_x() * 2.0f, w.rect.h - pad_y() * 2.0f};
                panel_clip = c;
                dl.push_clip(c.x, c.y, c.w, c.h);
                ++clip_depth;
                continue;
            }
            if (w.kind == WidgetKind::TitleBar && clip_depth > 0) {
                // 标题栏是窗口 chrome，不是内容：它占的是窗口最上面那一行，
                // 而面板推入的内容裁剪从 PAD_Y 之下才开始。不临时出界的话
                // 标题栏上半截被自己的裁剪切掉（症状：标题栏看起来"矮了 5px"、
                // 顶部露出窗口底色）。画完把内容裁剪补回来。
                dl.pop_clip();
                --clip_depth;
                ui_draw_widget(w, *theme_, *metrics_, ctx, dl);
                dl.push_clip(panel_clip.x, panel_clip.y, panel_clip.w, panel_clip.h);
                ++clip_depth;
                continue;
            }
            if (w.kind == WidgetKind::ClipBegin) {
                dl.push_clip(w.rect.x, w.rect.y, w.rect.w, w.rect.h);
                ++clip_depth;
                continue;
            }
            ui_draw_widget(w, *theme_, *metrics_, ctx, dl);
        }
        while (clip_depth > 0) { dl.pop_clip(); --clip_depth; }
    }
}

// ---------------------------------------------------------------------------
// 状态栈
// ---------------------------------------------------------------------------

BuiltinUi::WindowPersist* BuiltinUi::find_persist(uint64_t id) {
    for (int i = 0; i < persist_count_; ++i) {
        if (persist_[i].id == id) return &persist_[i];
    }
    if (persist_count_ >= kMaxWindows) return &persist_[0];   // 满了复用：不影响正确性
    WindowPersist* p = &persist_[persist_count_++];
    *p = WindowPersist{};
    p->id = id;
    return p;
}

BuiltinUi::WindowState* BuiltinUi::push_state(uint64_t id, const Rect& rect, uint32_t flags,
                                              bool is_popup, bool is_child) {
    if (depth_ >= kMaxWindows) return nullptr;
    WindowState& win = windows_[depth_];
    win = WindowState{};
    win.id      = id;
    win.rect    = rect;
    win.host_rect = rect;
    win.flags   = flags;
    win.is_popup = is_popup;
    win.is_child = is_child;
    win.layer   = is_popup ? 1 : ((depth_ > 0) ? windows_[depth_ - 1].layer : 0);
    win.title_h = 0.0f;
    // 缩进的继承分两种：
    //   子区域（BeginChild）从父级的缩进处起算 —— 嵌在折叠块里的子区域要
    //   跟着缩进走。
    //   弹层是**独立的窗口**，不继承调用点的缩进。检视器里"MeshRenderer"
    //   折叠块内的属性行会带一级缩进，弹层继承它的话，整个弹层内容会整体
    //   右移一格（表现为过滤框和列表没有贴着弹层左边）。
    win.indent_x = (depth_ > 0 && !is_popup) ? windows_[depth_ - 1].indent_x : 0.0f;
    ++depth_;
    return &windows_[depth_ - 1];
}

namespace {
// 弹层的内容命中域：内缩一个内边距
Rect popup_clip(const Rect& rect, float pad) {
    return Rect{rect.x + pad, rect.y + pad,
                (rect.w > pad * 2.0f) ? (rect.w - pad * 2.0f) : 0.0f,
                (rect.h > pad * 2.0f) ? (rect.h - pad * 2.0f) : 0.0f};
}
} // namespace

void BuiltinUi::pop_state() {
    if (depth_ <= 0) return;
    --depth_;
    if (depth_ <= 0) {
        tree_.clear_clip();
        return;
    }
    // 恢复父区域自己的命中域（父可能是窗口、弹层或子区域，各自的域不同）
    const WindowState& parent = windows_[depth_ - 1];
    if (parent.rect.w <= 0.0f) {
        tree_.clear_clip();
        return;
    }
    if (parent.is_popup) {
        tree_.set_clip(popup_clip(parent.rect, pad_x()));
    } else {
        const float view_h = (parent.rect.bottom() - pad_y()) - parent.content_top;
        tree_.set_clip(Rect{parent.rect.x + pad_x(), parent.content_top,
                            (parent.rect.w > pad_x() * 2.0f) ? (parent.rect.w - pad_x() * 2.0f) : 0.0f,
                            (view_h > 0.0f) ? view_h : 0.0f});
    }
}

void BuiltinUi::stamp(Widget* w) {
    if (!w || depth_ == 0) return;
    const WindowState& win = windows_[depth_ - 1];
    w->scope = win.id;
    w->layer = win.layer;
}

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------

int BuiltinUi::begin_window(const char* name, const Rect& rect, uint32_t flags) {
    if (depth_ >= kMaxWindows) return 0;
    // 退化矩形也压栈（零矩形），保证宿主照常写 Begin/End 配对；但它被标成
    // 不可用：内容提交全部 no-op，不产生任何控件。
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        WindowState* bad = push_state(hash_label(name), Rect{}, flags, false, false);
        if (bad) bad->valid = false;
        return 0;
    }

    const uint64_t id = hash_label(name);
    WindowPersist* p = find_persist(id);

    // 标题栏：与 ImGui 一致——**默认就有**，只有显式传 NO_TITLE_BAR 才没有。
    // 早期内置轨是反过来的（必须显式要），于是同一个宿主在两条轨下画出来的
    // 窗口结构不一样：ImGui 有标题栏没菜单栏、内置轨有菜单栏没标题栏。
    const bool want_title = (flags & kWinNoTitleBar) == 0;

    // 标题栏拖动：按下帧记"鼠标起点 + 当时的位移"，之后 offset = 起点位移 +
    // 鼠标位移。必须在算 eff 之前做，否则位移要晚一帧才反映到矩形上。
    if (want_title && !(flags & kWinNoMove)) {
        const Rect tr0{rect.x + p->offset_x, rect.y + p->offset_y, rect.w, frame_height()};
        const UiInputFrame& in0 = tree_.input();
        if (in0.mouse_clicked[0] && rect_contains(tr0, in0.mouse_x, in0.mouse_y) &&
            (in0.mouse_x >= tr0.x + 22.0f || (flags & kWinNoCollapse))) {
            p->drag_active   = true;
            p->drag_mouse_x  = in0.mouse_x;
            p->drag_mouse_y  = in0.mouse_y;
            p->drag_offset_x = p->offset_x;
            p->drag_offset_y = p->offset_y;
        }
        if (p->drag_active) {
            if (in0.mouse_down[0]) {
                p->offset_x = p->drag_offset_x + (in0.mouse_x - p->drag_mouse_x);
                p->offset_y = p->drag_offset_y + (in0.mouse_y - p->drag_mouse_y);
            } else {
                p->drag_active = false;   // 松手：位移留住，抓取状态清掉
            }
        }
    }

    Rect eff = rect;
    eff.x += p->offset_x;
    eff.y += p->offset_y;

    WindowState* win = push_state(id, eff, flags, false, false);
    if (!win) return 0;
    win->host_rect = rect;

    win->title_h = want_title ? frame_height() : 0.0f;
    win->content_top = eff.y + win->title_h + pad_y();

    const float view_h = (eff.bottom() - pad_y()) - win->content_top;
    win->collapsed = want_title && p->collapsed;

    // 悬停判据：落在窗口矩形里就不该把点击交给视口
    const UiInputFrame& in = tree_.input();
    const bool mouse_in = rect_contains(eff, in.mouse_x, in.mouse_y);
    if (mouse_in) mouse_in_window_ = true;

    // 滚轮：只在内容真的超出时吃（否则宿主还能拿滚轮做别的）
    if (mouse_in && !popup_.open && view_h > 0.0f) {
        const float max_scroll = (p->content_h > view_h) ? (p->content_h - view_h) : 0.0f;
        if (max_scroll > 0.0f && in.wheel != 0.0f) {
            p->scroll_y = clampf(p->scroll_y - in.wheel * line_height() * 2.0f, 0.0f, max_scroll);
        }
    }
    p->scroll_y = (view_h > 0.0f && p->content_h > view_h)
                ? clampf(p->scroll_y, 0.0f, p->content_h - view_h) : 0.0f;
    win->scroll_y = win->collapsed ? 0.0f : p->scroll_y;
    win->cursor_y = win->content_top - win->scroll_y;
    win->content_h = 0.0f;

    // 内容命中域（滚出可见区的条目不该还能点中）
    const Rect content{eff.x + pad_x(), win->content_top,
                       (eff.w > pad_x() * 2.0f) ? (eff.w - pad_x() * 2.0f) : 0.0f,
                       (view_h > 0.0f) ? view_h : 0.0f};

    // chrome 先按整个窗口矩形做命中域（标题栏在内容区之上，不能被内容域裁掉）
    tree_.set_clip(eff);

    // 背景板：不参与命中测试（点面板空白处要能清键盘焦点）。
    // 投影只给"带标题栏的浮窗"：固定分区的平铺面板互相贴着，阴影会画进
    // 邻居身下（看不见），白花几百个顶点还多一次叠绘。
    Widget* bg = tree_.touch(id, WidgetKind::Panel, eff, name);
    if (bg) {
        bg->scope = 0;
        bg->layer = win->layer;
        bg->shadow = want_title;
    }

    if (!want_title) {
        tree_.set_clip(content);
        return 1;
    }

    // 标题栏：可拖动；窗口允许折叠时左侧才有折叠框（ImGui 的 NoCollapse 同理）。
    if (win->collapsed) win->title_h = 0.0f;
    const Rect tr{eff.x, eff.y, eff.w, frame_height()};
    const uint64_t tid = id ^ 0x7E11ull;
    Widget* title = tree_.touch(tid, WidgetKind::TitleBar, tr, name);
    if (title) {
        title->open = !p->collapsed;
        title->fold_marker = (flags & kWinNoCollapse) == 0;
        title->scope = id;
        title->layer = win->layer;
        // 折叠：点在左侧小方块上
        if (title->clicked && in.mouse_x < tr.x + 22.0f && !(flags & kWinNoCollapse)) {
            p->collapsed = !p->collapsed;
            title->open = !p->collapsed;
        }
    }
    // 折叠态可能刚被这一帧的点击改掉（点击在松开帧成立），重新取值再决定
    win->collapsed = want_title && p->collapsed;
    if (win->collapsed) {
        win->title_h  = 0.0f;
        win->scroll_y = 0.0f;
        win->cursor_y = win->content_top;
        return 0;
    }

    // 内容命中域
    tree_.set_clip(content);
    return 1;
}

void BuiltinUi::end_window() {
    if (depth_ <= 0) return;
    WindowState& win = windows_[depth_ - 1];
    if (win.rect.w <= 0.0f || win.is_popup || win.is_child) {
        pop_state();
        return;
    }

    WindowPersist* p = find_persist(win.id);
    const float view_h = (win.rect.bottom() - pad_y()) - win.content_top;
    if (view_h > 0.0f) {
        const float max_scroll = (win.content_h > view_h) ? (win.content_h - view_h) : 0.0f;
        win.scroll_y = clampf(win.scroll_y, 0.0f, max_scroll);
    } else {
        win.scroll_y = 0.0f;
    }
    p->scroll_y   = win.scroll_y;
    p->content_h  = win.content_h;

    submit_scrollbar(win);
    pop_state();
}

void BuiltinUi::submit_scrollbar(WindowState& win) {
    if (win.flags & kWinNoScrollbar) return;
    if (win.content_top >= win.rect.bottom()) return;

    const float view_h = (win.rect.bottom() - pad_y()) - win.content_top;
    const float total  = win.content_h;
    if (view_h <= 0.0f || total <= view_h + 1.0f) return;

    const float sbw = (theme_ && THEME_METRIC_SCROLLBAR_SIZE < THEME_METRIC_COUNT)
                    ? theme_->metrics[THEME_METRIC_SCROLLBAR_SIZE] : 12.0f;
    const float right = win.rect.right() - pad_x();
    const Rect r{right - sbw, win.content_top, sbw, view_h};
    Widget* w = tree_.touch(hash_label("##vscrollbar", win.id), WidgetKind::ScrollBar, r, nullptr);
    if (!w) return;
    w->scope = win.id;
    w->layer = win.layer;
    w->thumb = clampf(view_h / total, 0.05f, 1.0f);
    const float max_scroll = total - view_h;
    w->value = (max_scroll > 0.0f) ? clampf(win.scroll_y / max_scroll, 0.0f, 1.0f) : 0.0f;
    // 拖动结果写回（下一帧生效：内容已经布好了，当帧无法回流）
    if (w->pressed) {
        win.scroll_y = w->value * max_scroll;
        WindowPersist* p = find_persist(win.id);
        p->scroll_y = win.scroll_y;
    }
}

Rect BuiltinUi::current_window_rect() const {
    return (depth_ > 0) ? windows_[depth_ - 1].rect : Rect{};
}

Rect BuiltinUi::window_rect_by_name(const char* name) const {
    const uint64_t id = hash_label(name);
    for (int i = 0; i < persist_count_; ++i) {
        if (persist_[i].id != id) continue;
        // 只有当前开着的窗口才知道宿主矩形
        for (int d = 0; d < depth_; ++d) {
            if (windows_[d].id != id) continue;
            Rect r = windows_[d].host_rect;
            r.x += persist_[i].offset_x;
            r.y += persist_[i].offset_y;
            return r;
        }
    }
    return Rect{};
}

float BuiltinUi::window_scroll_y() const {
    return (depth_ > 0) ? windows_[depth_ - 1].scroll_y : 0.0f;
}

void BuiltinUi::set_window_scroll_y(float v) {
    if (!usable()) return;
    WindowState& win = windows_[depth_ - 1];
    win.scroll_y = (v > 0.0f) ? v : 0.0f;
    WindowPersist* p = find_persist(win.id);
    p->scroll_y = win.scroll_y;
}

// ---------------------------------------------------------------------------
// 光标流
// ---------------------------------------------------------------------------

void BuiltinUi::same_line(float spacing) {
    if (!usable()) return;
    WindowState& win = windows_[depth_ - 1];
    win.pending_same_line = true;
    win.same_line_spacing = spacing;
}

void BuiltinUi::spacing(float px) {
    if (!usable()) return;
    // px < 0 = "用主题的行间距"（GUi_Spacing 的语义：空一行）；px == 0 是空操作。
    // 空一行走两步：ImGui 的 Spacing() 是 ItemSize(ItemSpacing)，即"一个高
    // ItemSpacing 的空行"再加上它后面那一条 ItemSpacing，所以是两倍。
    if (px < 0.0f) {
        windows_[depth_ - 1].cursor_y += item_spacing() * 2.0f;
        return;
    }
    if (px == 0.0f) return;
    windows_[depth_ - 1].cursor_y += px;
}

float BuiltinUi::indent_metric() const {
    return (theme_ && THEME_METRIC_INDENT < THEME_METRIC_COUNT)
         ? theme_->metrics[THEME_METRIC_INDENT] : 12.0f;
}

void BuiltinUi::indent(float px) {
    if (!usable()) return;
    windows_[depth_ - 1].indent_x += (px < 0.0f) ? indent_metric() : px;
}

void BuiltinUi::unindent(float px) {
    if (!usable()) return;
    WindowState& win = windows_[depth_ - 1];
    win.indent_x -= (px < 0.0f) ? indent_metric() : px;
    if (win.indent_x < 0.0f) win.indent_x = 0.0f;
}

Rect BuiltinUi::place(float w, float h) {
    if (!usable()) return Rect{};
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
        r = Rect{win.rect.x + pad_x() + win.indent_x, win.cursor_y, w, h};
        win.cursor_y += h + item_spacing();
    }

    // 右边界按内容区裁；宽度不够就缩到 0（不画也不交互，但光标照走）
    const float right = win.rect.right() - pad_x();
    if (r.x + r.w > right) r.w = right - r.x;
    const float avail = cw - win.indent_x;
    if (r.w > avail) r.w = avail;
    if (r.w < 0.0f) r.w = 0.0f;

    // 内容高度（未减滚动）：end_window 用它算滚动范围
    const float rel_bottom = r.bottom() - win.content_top + win.scroll_y;
    if (rel_bottom > win.content_h) win.content_h = rel_bottom;

    win.last_rect = r;
    return r;
}

// 菜单宽度自适应的一票：item 报"我至少要多宽"。只在**按内容自适应**的
// 菜单弹层里生效——固定宽度的弹层由调用方拍板，不该被内容顶大。
//
// 只认"文本需要多宽"（Menu/emit_label 报），不按 item 右边缘累计：后者在
// 宽度大于内容时会把自己报大，弹层就再也收不回来（第一帧用了估算宽度就
// 一直卡在那个宽度）。代价是图片 / 占位一类没有文本的 item 不参与定宽，
// 菜单里也基本不会出现。
void BuiltinUi::note_menu_width(float w) {
    if (!popup_.open || !popup_.is_menu || popup_.w > 0.0f) return;
    if (depth_ <= 0 || windows_[depth_ - 1].id != popup_.id) return;
    // 上限：允许菜单为一行说明文字宽一点，但不能让它吃着屏幕长大。
    float cap = (display_w_ > 0.0f) ? display_w_ * 0.6f : 480.0f;
    if (cap < popup_.min_w) cap = popup_.min_w;
    if (w > cap) w = cap;
    if (w > popup_.want_w) popup_.want_w = w;
}

// 菜单宽度缓存：同一个菜单第二次打开就是准的，不必先窄一帧再撑开。
void BuiltinUi::remember_menu_width(uint64_t id, float w) {
    if (id == 0 || w <= 1.0f) return;
    for (int i = 0; i < kMenuWidthCache; ++i) {
        if (menu_w_cache_[i].id == id) { menu_w_cache_[i].w = w; return; }
    }
    // 空位优先，其次覆盖最早写入的（菜单数量很小，不必做 LRU）
    int slot = -1;
    for (int i = 0; i < kMenuWidthCache; ++i) {
        if (menu_w_cache_[i].id == 0) { slot = i; break; }
    }
    if (slot < 0) slot = static_cast<int>(menu_w_seq_++ % kMenuWidthCache);
    menu_w_cache_[slot].id = id;
    menu_w_cache_[slot].w  = w;
}

float BuiltinUi::cached_menu_width(uint64_t id) const {
    for (int i = 0; i < kMenuWidthCache; ++i) {
        if (menu_w_cache_[i].id == id) return menu_w_cache_[i].w;
    }
    return 0.0f;
}

// 打开一个菜单弹层：宽度按内容自适应（下限 = 标题宽），起始宽度取缓存。
void BuiltinUi::open_menu_popup(uint64_t id, float x, float y, float title_w) {
    open_popup_id(id, x, y, 0.0f, 0.0f, false);
    popup_.is_menu = true;
    popup_.min_w   = title_w;
    const float cached = cached_menu_width(id);
    if (cached > title_w) popup_.measured_w = cached;
}

// ---------------------------------------------------------------------------
// 文本
// ---------------------------------------------------------------------------

void BuiltinUi::finish_impl(Widget* w) {
    if (!w) {
        last_rect_ = Rect{};
        last_hovered_ = false;
        last_pressed_ = false;
        last_id_ = 0;
        return;
    }
    last_rect_ = w->rect;
    last_hovered_ = w->hovered;
    last_pressed_ = w->pressed;
    last_id_ = w->id;
}

bool BuiltinUi::is_item_clicked(int button) const {
    if (button < 0 || button > 2) return false;
    if (!last_hovered_ && button == 0) return false;
    return tree_.input().mouse_clicked[button];
}

bool BuiltinUi::take_item_deactivated_after_edit() {
    if (last_id_ == 0) return false;
    Widget* w = tree_.find(last_id_);
    if (!w || !w->deactivated_after_edit) return false;
    // 查询即消费：宿主每帧都会问一次，不清就会连着两帧报同一笔编辑，
    // 落到撤销栈上就是"改一次名字出现两步"。
    w->deactivated_after_edit = false;
    w->deactivated_frame = 0;
    return true;
}

void BuiltinUi::text(const char* s) {
    if (!usable()) { finish_impl(nullptr); return; }
    text_wrapped(s, 0, false);
}

void BuiltinUi::text_colored(uint32_t rgba, const char* s) {
    if (!usable()) { finish_impl(nullptr); return; }
    text_wrapped(s, rgba, true);
}

// 单行 Label：几何 + 着色都在这里，wrap 出来的每一行也走它。
void BuiltinUi::emit_label(const char* s, uint32_t rgba, bool colored) {
    // 宽度取**实测文本宽**，不是整行宽：SameLine 之后的下一个 item 是从
    // last_rect.right() 接着摆的，Label 撑满整行就会把后面的按钮推到面板
    // 外面去（工具栏上的 "Save | Undo | Redo" 只剩 Save 就是这么来的）。
    // 没有度量器时退回整行宽——那时也算不出宽度，至少不改变原有布局。
    float label_w = content_width();
    if (metrics_) {
        const float tw = metrics_->width(s ? s : "", kWidgetFontSize);
        if (tw > 0.0f && tw < label_w) label_w = tw;
        // 菜单里的一行说明文字也参与"菜单要多宽"：用**未截断**的文本宽，
        // 否则最宽的那行会被当前（偏窄的）弹层宽度自己截住，永远长不大。
        note_menu_width(tw + pad_x() * 2.0f);
    }
    const Rect r = place(label_w, line_height());
    Widget* w = tree_.touch(item_id(s, true), WidgetKind::Label, r, s);
    if (w) {
        stamp(w);
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
    if (!usable()) { finish_impl(nullptr); return; }
    char buf[kWidgetLabelCap];
    std::snprintf(buf, sizeof(buf), "%s: %s", label ? label : "", text ? text : "");
    const Rect r = place(content_width(), frame_height());
    Widget* w = tree_.touch(item_id(label, true), WidgetKind::Label, r, buf);
    if (w) stamp(w);
    finish_impl(w);
}

void BuiltinUi::separator() {
    if (!usable()) { finish_impl(nullptr); return; }
    const Rect r = place(content_width(), 1.0f);
    Widget* w = tree_.touch(item_id("##separator", true), WidgetKind::Separator, r, nullptr);
    if (w) stamp(w);
    finish_impl(w);
}

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

int BuiltinUi::button(const char* label) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
    const float w = tw + pad_x() * 2.0f;
    const Rect r = place(w, frame_height());
    Widget* wd = tree_.touch(item_id(label, false), WidgetKind::Button, r, shown);
    if (wd) stamp(wd);
    finish_impl(wd);
    return (wd && wd->clicked) ? 1 : 0;
}

int BuiltinUi::button_size(const char* label, float w, float h) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    if (w <= 0.0f) {
        const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
        w = tw + pad_x() * 2.0f;
    }
    if (h <= 0.0f) h = frame_height();
    const Rect r = place(w, h);
    Widget* wd = tree_.touch(item_id(label, false), WidgetKind::Button, r, shown);
    if (wd) stamp(wd);
    finish_impl(wd);
    return (wd && wd->clicked) ? 1 : 0;
}

int BuiltinUi::checkbox(const char* label, int* inout_value) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const bool was = (inout_value != nullptr && *inout_value != 0);

    // 先 ensure + 同步调用方的值（权威在调用方），再 touch 跑交互
    Widget* w = tree_.ensure(id, WidgetKind::Checkbox, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->checked = was;
    w = tree_.touch(id, WidgetKind::Checkbox, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    if (w->checked != was) {
        if (inout_value) *inout_value = w->checked ? 1 : 0;
        return 1;
    }
    return 0;
}

int BuiltinUi::radio_button(const char* label, int active) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    // 宽度按内容走：圆环 + 内间距 + 字宽。用 item_width()（65%）就没法在
    // 工具栏里横排——一个按钮会把整行吃掉。
    // 圆环直径 = 字号（ImGui 的 RadioButton 用 radius = FontSize * 0.5），
    // 比复选框的方框（= 行高）小一圈，视觉上与 ImGui 同一档。
    const float tw  = (shown[0] && metrics_) ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
    const float dia = kWidgetFontSize;
    const Rect r = place(dia + (tw > 0.0f ? tw + kItemInnerSpacing : 0.0f), frame_height());
    Widget* w = tree_.touch(item_id(label, false), WidgetKind::Radio, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->selected = active != 0;
    finish_impl(w);
    return w->clicked ? 1 : 0;
}

int BuiltinUi::slider_float(const char* label, float* inout_value, float vmin, float vmax,
                            const char* fmt) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const float was = inout_value ? *inout_value : 0.0f;

    Widget* w = tree_.ensure(id, WidgetKind::Slider, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->min_value = vmin;
    w->max_value = (vmax > vmin) ? vmax : vmin + 1.0f;
    w->value = was;
    std::snprintf(w->fmt, sizeof(w->fmt), "%s", (fmt && fmt[0]) ? fmt : "%.3f");
    w = tree_.touch(id, WidgetKind::Slider, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    if (w->value != was) {
        if (inout_value) *inout_value = w->value;
        return 1;
    }
    return 0;
}

int BuiltinUi::drag_float(const char* label, float* inout_value, float speed,
                          float vmin, float vmax) {
    if (depth_ == 0 || !inout_value) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const float was = *inout_value;

    Widget* w = tree_.ensure(id, WidgetKind::DragFloat, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->value     = was;
    w->min_value = vmin;
    w->max_value = (vmax > vmin) ? vmax : vmin;
    tree_.set_drag_speed((speed > 0.0f) ? speed : 1.0f);
    w = tree_.touch(id, WidgetKind::DragFloat, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    if (w->value != was) {
        *inout_value = w->value;
        return 1;
    }
    return 0;
}

int BuiltinUi::selectable(const char* label, int selected) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    // 整行宽（ImGui 的 Selectable 默认填满可用宽度）：列表项靠左对齐成一列，
    // 宽度不齐的话选中高亮块长短不一，看着像没对齐。
    const Rect r = place(content_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const bool was = (selected != 0);

    // 先 ensure + 同步调用方的选中态（权威在调用方），再 touch 跑交互
    Widget* w = tree_.ensure(id, WidgetKind::Selectable, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->selected = was;
    w = tree_.touch(id, WidgetKind::Selectable, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    // 返回值只表示"被点了"，不改选中态（与 ImGui::Selectable(label, bool) 一致）。
    return (w->clicked) ? 1 : 0;
}

// N 分量拖拽（ImGui 的 DragScalarN 语义）：一行里并排 N 个只显示数值的框，
// 标签文本跟在**整组**的右边。
//
// 为什么不用 N 个独立的 drag_float 提交：那会把它们排成 N **行**，一行只
// 能放一个框。这里改为整组占位一次（place 只推进一次光标），再把这个矩形
// 手工切分成 N 份。
//
// 拖哪个改哪个由 WidgetTree 保证：drag_owner_ 按控件 id 判定归属，且
// active_id_ 全树唯一（同一帧只有一个控件能 pressed）——N 个并排的框天然
// 互不干扰。
int BuiltinUi::drag_float_n(const char* label, float* inout_values, int count,
                            float speed) {
    if (!usable() || !inout_values || count < 1 || count > 4) {
        finish_impl(nullptr);
        return 0;
    }
    if (count == 1) return drag_float(label, inout_values, speed, 0.0f, 0.0f);

    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const float spacing = item_spacing();

    // 控件组只占 item_width()，标签画在组**右侧**（与 ImGui 的 DragScalarN 一致：
    // 组宽取 ItemWidth，标签用 ItemInnerSpacing 跟在后面，不占组宽）。这样宿主
    // 用 SetNextItemWidth(-reserve) 就能把右边留给标签；早先这里按整行占位、
    // 再从行内切出标签的位置，会让 SetNextItemWidth 完全失效 —— 检视器的
    // Position 三框吃满整宽，标签被挤到面板边缘只剩半个字。
    const Rect row = place(item_width(), frame_height());
    if (row.w <= 0.0f || row.h <= 0.0f) { finish_impl(nullptr); return 0; }

    const float box_w = (row.w - spacing * (count - 1)) / static_cast<float>(count);
    if (box_w <= 0.0f) { finish_impl(nullptr); return 0; }

    int      changed = 0;
    bool     any_hovered = false;
    bool     any_pressed = false;

    for (int i = 0; i < count; ++i) {
        // id 由「标签 + 分量序号」派生：同一组里的 N 个框必须是 N 个不同控件，
        // 且跨帧稳定（否则拖到一半丢状态）。
        char idbuf[kWidgetLabelCap + 16];
        std::snprintf(idbuf, sizeof(idbuf), "%s##%d", label ? label : "", i);
        const uint64_t id = item_id(idbuf, false);

        const Rect sub{row.x + static_cast<float>(i) * (box_w + spacing),
                       row.y, box_w, row.h};
        float* vp = inout_values + i;
        const float was = *vp;

        Widget* w = tree_.ensure(id, WidgetKind::DragFloat, sub, "");
        if (!w) continue;
        stamp(w);
        w->value     = was;
        w->min_value = 0.0f;
        w->max_value = 0.0f;   // 不限幅（与 GUi_DragFloat 的内置轨行为一致）
        tree_.set_drag_speed((speed > 0.0f) ? speed : 1.0f);
        w = tree_.touch(id, WidgetKind::DragFloat, sub, "");
        if (!w) continue;
        stamp(w);
        if (w->hovered) any_hovered = true;
        if (w->pressed) any_pressed = true;
        if (w->value != was) {
            *vp = w->value;
            changed = 1;
        }
    }

    // 组标签：纯显示，不参与命中测试。位置 = 组右沿 + ItemInnerSpacing，
    // 与单分量 DragFloat 的 draw_label_right 同一口径（4px）。
    if (shown[0]) {
        const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
        const Rect lr{row.right() + 4.0f, row.y, tw, row.h};
        Widget* lw = tree_.touch(item_id(label, true), WidgetKind::Label, lr, shown);
        if (lw) stamp(lw);
    }

    // 收尾状态按"整组"报：ImGui 的 group 语义下 LastItem 是组本身。
    last_rect_    = row;
    last_hovered_ = any_hovered;
    last_pressed_ = any_pressed;
    last_id_      = 0;   // 组不是拖放源（避免误判成拖起来的 item）
    return changed;
}

int BuiltinUi::input_text(const char* label, char* buf, int buf_size) {
    if (depth_ == 0 || !buf || buf_size <= 1) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);

    Widget* w = tree_.ensure(id, WidgetKind::TextInput, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    // 调用方缓冲区 → 控件（超出控件容量的部分截断；焦点/编辑态在树里）。
    // 只在内容真的变了时才整体覆盖：否则每帧覆盖会吃掉这一帧的编辑结果，
    // 插入点与选区也会被冲掉。
    if (std::strncmp(w->text, buf, sizeof(w->text) - 1) != 0) {
        std::strncpy(w->text, buf, sizeof(w->text) - 1);
        w->text[sizeof(w->text) - 1] = '\0';
        if (w->caret > static_cast<int>(std::strlen(w->text))) {
            w->caret = static_cast<int>(std::strlen(w->text));
        }
    }

    char before[kWidgetTextCap];
    std::snprintf(before, sizeof(before), "%s", w->text);

    w = tree_.touch(id, WidgetKind::TextInput, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    // 上一行若调过 GUi_SetKeyboardFocusHere，焦点落在这个框上（插入点末尾）。
    // 放在 touch 之后：touch 内部可能因为"点了别的控件"把焦点清掉，顺序颠倒
    // 会让请求被自己这一步吞掉。
    if (focus_next_) {
        tree_.focus_widget(id);
        focus_next_ = false;
    }
    finish_impl(w);

    // 读回（同样按调用方容量截断）
    std::strncpy(buf, w->text, static_cast<size_t>(buf_size) - 1);
    buf[buf_size - 1] = '\0';
    return (std::strcmp(before, w->text) != 0) ? 1 : 0;
}

void BuiltinUi::progress_bar(float fraction, const char* overlay) {
    if (!usable()) { finish_impl(nullptr); return; }
    const Rect r = place(content_width(), frame_height());
    Widget* w = tree_.touch(item_id(overlay ? overlay : "##progress", true),
                            WidgetKind::Progress, r, overlay);
    if (w) {
        stamp(w);
        w->value = fraction;
    }
    finish_impl(w);
}

// 图片（内置轨的 GUi_Image）。tex 是打包的引擎纹理句柄，由
// GRender_GetUiTextureId 在内置轨下取得——与 ImGui 轨的 ImTextureID 是两套
// id 空间，宿主必须按当前 UI 轨取对应的那个（见 ui-api.md 7.11）。
void BuiltinUi::image(uint64_t tex, float w, float h) {
    if (!usable() || tex == 0 || w <= 0.0f || h <= 0.0f) {
        finish_impl(nullptr);
        return;
    }

    // id 由纹理句柄派生：同一窗口里同一张图必须是同一个控件（悬停/按下状态才
    // 跨帧连续），不同图天然是不同控件。同窗口重复贴同一张图会共用一个 id
    // （后提交的覆盖几何）——ImGui 那边更退化（Image 的 id 恒为 0，全靠
    // LastItemRect 兜底），编辑器一帧只贴一张视口图，不触及这个角落。
    char idbuf[kWidgetLabelCap];
    std::snprintf(idbuf, sizeof(idbuf), "__img:%llu",
                  static_cast<unsigned long long>(tex));
    const Rect r = place(w, h);
    const uint64_t id = item_id(idbuf, false);

    Widget* iw = tree_.ensure(id, WidgetKind::Image, r, "");
    if (!iw) { finish_impl(nullptr); return; }
    stamp(iw);
    iw->texture = tex;
    // V 方向按后端采样约定在建控件时算好（OpenGL 的 framebuffer 原点在左下，
    // 纹理第 0 行是图像的下边；Vulkan / D3D12 在上边）。绘制端不再判断后端。
    // 判据只能在这里读：UI 回调运行在渲染锁已持有的路径上，不能反查渲染器。
    iw->u0 = 0.0f;
    iw->v0 = texture_v_flip() ? 1.0f : 0.0f;
    iw->u1 = 1.0f;
    iw->v1 = texture_v_flip() ? 0.0f : 1.0f;
    iw = tree_.touch(id, WidgetKind::Image, r, "");
    if (!iw) { finish_impl(nullptr); return; }
    stamp(iw);
    finish_impl(iw);
}

int BuiltinUi::color_button(const char* label, const float rgba[4]) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    // 与 ImGui 一致：ColorButton 的标签只当 id 用，不出现在界面上（块内就是
    // 色块本身）。所以这里给控件空标签，但 id 仍取完整 label。
    const float side = frame_height();
    const Rect r = place(side, side);
    Widget* w = tree_.touch(item_id(label, false), WidgetKind::Button, r, "");
    if (w) {
        stamp(w);
        w->bg_overridden = true;
        w->bg_color = pack_rgba(rgba);
    }
    finish_impl(w);
    return (w && w->clicked) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 下拉框 / 颜色编辑（与 ImGui 的 Combo / ColorEdit4 同布局）
// ---------------------------------------------------------------------------

int BuiltinUi::combo(const char* label, int* current, const char* const* items, int count) {
    if (!usable() || !current || !items || count <= 0) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);
    const int cur = (*current < 0 || *current >= count) ? 0 : *current;

    Widget* w = tree_.touch(id, WidgetKind::Combo, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    // 框内显示当前项（ImGui 的 preview_value）
    std::snprintf(w->text, sizeof(w->text), "%s", items[cur] ? items[cur] : "");
    finish_impl(w);

    // 弹层列表：位置贴框的下沿、宽度取框宽（ImGui 的 BeginComboPopup 同理）
    char popup_id[96];
    std::snprintf(popup_id, sizeof(popup_id), "##combo_%s", label ? label : "c");
    if (w->clicked) {
        open_popup(popup_id, r.x, r.bottom(), r.w, 0.0f);
    }

    int changed = 0;
    if (begin_popup(popup_id)) {
        for (int i = 0; i < count; ++i) {
            if (menu_item(items[i] ? items[i] : "", nullptr, 1)) {
                *current = i;
                changed = 1;
            }
        }
        end_popup();
    }
    return changed;
}

int BuiltinUi::color_edit(const char* label, float rgba[4]) {
    if (!usable() || !rgba) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    const uint64_t id = item_id(label, false);
    float was[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};

    // 先 ensure + 同步调用方的颜色（权威在调用方），再 touch 跑交互
    Widget* w = tree_.ensure(id, WidgetKind::ColorEdit, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    for (int i = 0; i < 4; ++i) w->color[i] = was[i];
    w = tree_.touch(id, WidgetKind::ColorEdit, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    int changed = 0;
    for (int i = 0; i < 4; ++i) {
        const float v = clampf(w->color[i], 0.0f, 1.0f);
        rgba[i] = v;
        if (v != was[i]) changed = 1;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// 结构：树 / 折叠头 / 菜单 / 子区域
// ---------------------------------------------------------------------------
int BuiltinUi::tree_node(const char* label, int default_open) {
    return tree_node_ex(label, default_open ? kTreeNodeDefaultOpen : 0);
}

int BuiltinUi::tree_node_ex(const char* label, int flags) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    WindowState& win = windows_[depth_ - 1];
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
    // 铺满整行的高亮：宽度取内容区剩余宽（place 会再按 avail 夹一次）。
    const float w = (flags & kTreeNodeSpanAvail) ? content_width()
                                                 : (kTreeNodeArrowW + tw);
    const Rect r = place(w, line_height());
    const uint64_t id = item_id(label, false);

    // open 是"树自己持有的状态"：只有首次创建时才用 default_open 初始化，
    // 之后不再覆盖（否则宿主每帧传同一个 default 会把展开态按回去）。
    if (!tree_.find(id)) {
        Widget* fresh = tree_.ensure(id, WidgetKind::TreeNode, r, shown);
        if (fresh) { stamp(fresh); fresh->open = (flags & kTreeNodeDefaultOpen) != 0; }
    }
    Widget* node = tree_.touch(id, WidgetKind::TreeNode, r, shown);
    if (!node) { finish_impl(nullptr); return 0; }
    stamp(node);
    node->node_flags = static_cast<uint32_t>(flags);
    node->selected   = (flags & kTreeNodeSelected) != 0;
    finish_impl(node);

    const bool leaf = (flags & kTreeNodeLeaf) != 0;
    if (leaf) return 1;           // 叶子恒"可见"，也不推缩进
    if (!node->open) return 0;
    win.indent_x += indent_metric();
    ++win.indent_depth;
    return 1;
}

void BuiltinUi::tree_pop() {
    if (!usable()) return;
    WindowState& win = windows_[depth_ - 1];
    if (win.indent_depth <= 0) return;
    --win.indent_depth;
    win.indent_x -= indent_metric();
    if (win.indent_x < 0.0f) win.indent_x = 0.0f;
}

int BuiltinUi::collapsing_header(const char* label, int default_open) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    WindowState& win = windows_[depth_ - 1];
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(content_width(), frame_height());
    const uint64_t id = item_id(label, false);
    if (!tree_.find(id)) {
        Widget* fresh = tree_.ensure(id, WidgetKind::Header, r, shown);
        if (fresh) { stamp(fresh); fresh->open = default_open != 0; }
    }
    Widget* w = tree_.touch(id, WidgetKind::Header, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    finish_impl(w);

    const bool open = w->open;
    if (open) {
        win.indent_x += indent_metric();
        ++win.indent_depth;
    }
    return open ? 1 : 0;
}

int BuiltinUi::begin_menu_bar() {
    if (!usable()) return 0;
    WindowState& win = windows_[depth_ - 1];
    // 与 ImGui 同一条规则：窗口**没有** MenuBar 标志时整条菜单栏不画，
    // 返回 0 让调用方跳过菜单体。ImGui 的 BeginMenuBar 也是这么做的
    // （它是"这个窗口有没有声明菜单栏"的判据，不是"现在要不要画"）。
    if ((win.flags & kWinMenuBar) == 0) return 0;

    const Rect bar = place(content_width(), frame_height());
    Widget* w = tree_.touch(item_id("##menubar", true), WidgetKind::MenuBar, bar, nullptr);
    if (w) {
        stamp(w);
        // 底色取主题的 MENUBAR_BG（与 ImGui 的 ImGuiCol_MenuBarBg 同一个
        // token）。角半径为 0、不描边：菜单栏是窗口 chrome，描边会把它读成
        // 一块面板。
        w->bg_overridden = true;
        w->bg_color = (theme_ && THEME_COLOR_MENUBAR_BG < THEME_COLOR_COUNT)
                    ? theme_->colors[THEME_COLOR_MENUBAR_BG] : 0u;
        w->corner_radius = 0.0f;
    }

    // 悬停切换：已经有菜单展开、鼠标移到同级标题上，就直接换过去（ImGui 的
    // 菜单栏行为）。做在这里而不是 begin_menu 里，是因为 BeginMenu 是边提交
    // 边判的——轮到第二个标题时第一个已经把自己的弹层提交了，切换会让同一帧
    // 出现两个菜单。放在 BeginMenuBar 就只有一个。
    if (popup_.open && popup_.is_menu) {
        for (int i = 0; i < menu_title_count_; ++i) {
            const MenuTitleHit& hit = menu_titles_[i];
            if (hit.popup_id == popup_.id) continue;
            if (!rect_contains(hit.rect, mouse_x_, mouse_y_)) continue;
            open_menu_popup(hit.popup_id, hit.rect.x, hit.rect.bottom(), hit.rect.w);
            break;
        }
    }
    menu_title_count_ = 0;

    // 标题从 bar 左边起，由菜单栏自己一个个摆开（见 begin_menu）
    win.in_menubar   = true;
    win.menubar_rect = bar;
    win.menubar_x    = bar.x;
    return 1;
}

void BuiltinUi::end_menu_bar() {
    if (!usable()) return;
    windows_[depth_ - 1].in_menubar = false;
}

namespace {
// 菜单的弹层 id 由标签派生（与按钮 id 区分开）
void make_menu_popup_id(const char* label, char* out, size_t cap) {
    std::snprintf(out, cap, "##menu_%s", label ? label : "");
}
} // namespace

int BuiltinUi::begin_menu(const char* label) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    WindowState& win = windows_[depth_ - 1];
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const float pad = pad_x();
    const float tw  = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
    // 标题宽 = 字宽 + 左右内边距（ImGui 的 BeginMenu 用 FramePadding.x*2）。
    const float menu_w = tw + pad * 2.0f;

    Rect r;
    if (win.in_menubar) {
        // 菜单栏里的标题由菜单栏布局摆位：横排、紧密相接、不进光标流。
        // 借 SameLine 摆的话，"第几个菜单"只活在 pending_same_line 里，
        // 第二个菜单起就得调用方每帧补一次置位（漏了就是竖着摞），而且会
        // 跟宿主自己的 SameLine / 换行互相污染。
        const float right = win.menubar_rect.right();
        float x = win.menubar_x;
        if (x + menu_w > right) x = right - menu_w;      // 放不下就贴右边
        if (x < win.menubar_rect.x) x = win.menubar_rect.x;
        r = Rect{x, win.menubar_rect.y, menu_w, win.menubar_rect.h};
        win.menubar_x = r.right();
    } else {
        // 弹层里的子菜单 / 独立菜单：走普通光标流，整行宽。
        r = place(item_width(), frame_height());
    }

    Widget* w = tree_.touch(item_id(label, false), WidgetKind::Menu, r, shown);
    if (w) stamp(w);

    char pid[64];
    make_menu_popup_id(label, pid, sizeof(pid));
    const uint64_t popup_id = hash_label(pid);
    const bool was_open = popup_.open && popup_.id == popup_id;

    // 记下标题矩形，供下一帧的悬停切换判位（只在菜单栏里有意义）
    if (win.in_menubar && menu_title_count_ < kMaxMenuTitles) {
        menu_titles_[menu_title_count_].popup_id = popup_id;
        menu_titles_[menu_title_count_].rect     = r;
        ++menu_title_count_;
    }

    if (w && w->clicked) {
        // 这一次点击是不是刚把菜单关掉（begin_frame 判的"点外面"）？是就别再开，
        // 否则"点自己的标题关菜单"会变成关了立刻又开。
        const bool closed_here = dismiss_x_ >= r.x && dismiss_x_ <= r.right() &&
                                 dismiss_y_ >= r.y && dismiss_y_ <= r.bottom();
        if (closed_here) {
            // 保持关闭
        } else if (was_open) {
            popup_.open = false;      // 再点一次 = 收起来
        } else {
            open_menu_popup(popup_id, r.x, r.bottom(), menu_w);
        }
    }

    const bool is_open = popup_.open && popup_.id == popup_id;
    if (w) w->open = is_open;     // 展开态决定标题底色（Header 恒亮）
    finish_impl(w);

    if (is_open && begin_popup_id(popup_id)) {
        menu_begun_ = true;
        return 1;
    }
    menu_begun_ = false;
    return 0;
}

void BuiltinUi::end_menu() {
    if (!menu_begun_) return;
    end_popup();
    menu_begun_ = false;
}

int BuiltinUi::menu_item(const char* label, const char* shortcut, int enabled) {
    if (!usable()) { finish_impl(nullptr); return 0; }
    char shown[kWidgetLabelCap];
    copy_display_label(label, shown, sizeof(shown));
    const Rect r = place(item_width(), frame_height());
    Widget* w = tree_.touch(item_id(label, false), WidgetKind::MenuItem, r, shown);
    if (!w) { finish_impl(nullptr); return 0; }
    stamp(w);
    w->enabled = enabled != 0;
    w->text[0] = '\0';
    if (shortcut) {
        std::strncpy(w->text, shortcut, sizeof(w->text) - 1);
        w->text[sizeof(w->text) - 1] = '\0';
    }
    // 菜单宽度由最宽的一项决定：标签 + 右对齐的快捷键。标题宽度算不出来，
    // 只能在这里累计（见 note_menu_width / PopupState::want_w）。
    {
        const float tw = metrics_ ? metrics_->width(shown, kWidgetFontSize) : 0.0f;
        const float sw = (shortcut && metrics_) ? metrics_->width(shortcut, kWidgetFontSize) : 0.0f;
        note_menu_width(tw + ((sw > 0.0f) ? (sw + 16.0f) : 0.0f) + pad_x() * 2.0f);
    }
    finish_impl(w);
    const bool clicked = w && w->clicked;
    if (clicked) close_current_popup();
    return clicked ? 1 : 0;
}

int BuiltinUi::begin_child(const char* id, float w, float h, int border) {
    if (!usable()) return 0;
    WindowState& parent = windows_[depth_ - 1];
    const float cw = content_width() - parent.indent_x;
    if (w <= 0.0f || w > cw) w = cw;
    if (h <= 0.0f) {
        // 与 ImGui 的 BeginChild(size=(0,0)) 一致：占满剩余空间。给一个
        // 固定的小高度会让子区域的裁剪把后面的内容全切掉。
        const float bottom = parent.rect.bottom() - pad_y();
        const float left = bottom - parent.cursor_y;
        h = (left > 0.0f) ? left : line_height() * 8.0f;
    }
    const Rect r = place(w, h);

    const uint64_t cid = hash_label(id, parent.id);
    WindowPersist* p = find_persist(cid);
    const float view_h = r.h - pad_y() * 2.0f;
    p->scroll_y = (view_h > 0.0f && p->content_h > view_h)
                ? clampf(p->scroll_y, 0.0f, p->content_h - view_h) : 0.0f;

    // 边框 + 底：纯装饰，不切裁剪（Panel 会切，这里不能让父窗口的裁剪失配）
    if (border) {
        Widget* bg = tree_.touch(cid ^ 0x51ull, WidgetKind::Frame, r, nullptr);
        if (bg) { bg->scope = parent.id; bg->layer = parent.layer; }
    }

    WindowState* win = push_state(cid, r, kWinNoTitleBar, false, true);
    if (!win) { finish_impl(nullptr); return 0; }
    win->content_top = r.y + pad_y();
    win->cursor_y    = win->content_top - p->scroll_y;
    win->scroll_y    = p->scroll_y;
    win->content_h   = 0.0f;
    win->layer       = parent.layer;

    // 子区域自己的裁剪（内缩边框）。两个 widget 分别管"命中"与"绘制"：
    // set_clip 只盖在控件的命中域上（滚出可见区点不中），ClipBegin/ClipEnd
    // 才是 draw 时的真裁剪——少了它内容会画到盒子外面去。
    const Rect inner{r.x + 1.0f, win->content_top,
                     r.w - 2.0f - pad_x() * 2.0f, view_h};
    tree_.set_clip(inner);
    if (view_h > 0.0f) {
        Widget* cb = tree_.touch(cid ^ 0x5C1ull, WidgetKind::ClipBegin, inner, nullptr);
        if (cb) { cb->scope = parent.id; cb->layer = parent.layer; }
    }

    // 子区域也吃滚轮
    const UiInputFrame& in = tree_.input();
    if (rect_contains(r, in.mouse_x, in.mouse_y) && view_h > 0.0f && in.wheel != 0.0f) {
        const float max_scroll = (p->content_h > view_h) ? (p->content_h - view_h) : 0.0f;
        if (max_scroll > 0.0f) {
            p->scroll_y = clampf(p->scroll_y - in.wheel * line_height() * 2.0f, 0.0f, max_scroll);
            win->scroll_y = p->scroll_y;
            win->cursor_y = win->content_top - p->scroll_y;
        }
    }
    return 1;
}

void BuiltinUi::end_child() {
    if (depth_ <= 0) return;
    WindowState& win = windows_[depth_ - 1];
    if (!win.is_child) return;
    WindowPersist* p = find_persist(win.id);
    p->content_h = win.content_h;
    p->scroll_y  = win.scroll_y;
    // 子区域内容超出可见高度时同样给一条滚动条（ImGui 的 BeginChild 默认带
    // 滚动条）：不给的话超出的最后几行既看不见也拖不动，日志面板会"少一行"。
    submit_scrollbar(win);
    // 收尾的裁剪标记（与 begin_child 的 ClipBegin 配对，只在有可见高度时才有）
    const float view_h = (win.rect.bottom() - pad_y()) - win.content_top;
    if (view_h > 0.0f) {
        const Rect inner{win.rect.x + 1.0f, win.content_top,
                         win.rect.w - 2.0f - pad_x() * 2.0f, view_h};
        Widget* ce = tree_.touch(win.id ^ 0x5C2ull, WidgetKind::ClipEnd, inner, nullptr);
        if (ce) { ce->scope = win.id; ce->layer = win.layer; }
    }
    pop_state();
}

// ---------------------------------------------------------------------------
// 弹层
// ---------------------------------------------------------------------------

void BuiltinUi::open_popup_id(uint64_t id, float x, float y, float w, float h, bool modal) {
    popup_ = PopupState{};
    popup_.id = id;
    popup_.open = true;
    popup_.modal = modal;
    popup_.w = w;
    popup_.h = h;
    // 位置夹到屏幕内（放不下时贴边，不让人拉不到菜单项）
    const float dw = display_w_ > 0.0f ? display_w_ : (current_window_rect().w + x);
    const float dh = display_h_ > 0.0f ? display_h_ : (current_window_rect().h + y);
    const float pw = (w > 0.0f) ? w : 180.0f;
    const float ph_est = (h > 0.0f) ? h : (frame_height() * 3.0f);
    if (dw > 0.0f) x = clampf(x, 0.0f, (dw - pw > 0.0f) ? (dw - pw) : 0.0f);
    if (dh > 0.0f) y = clampf(y, 0.0f, (dh - ph_est > 0.0f) ? (dh - ph_est) : 0.0f);
    popup_.x = x;
    popup_.y = y;
}

void BuiltinUi::open_popup(const char* id, float x, float y, float w, float h) {
    open_popup_id(hash_label(id), x, y, w, h, false);
}

void BuiltinUi::open_popup_on_item(const char* id, float w) {
    const Rect r = last_rect_;
    open_popup_id(hash_label(id), r.x, r.bottom(), w, 0.0f, false);
}

void BuiltinUi::open_modal(const char* name, float w, float h) {
    const float dw = display_w_ > 0.0f ? display_w_ : 800.0f;
    const float dh = display_h_ > 0.0f ? display_h_ : 600.0f;
    open_popup_id(hash_label(name), (dw - w) * 0.5f, (dh - h) * 0.5f, w, h, true);
}

int BuiltinUi::begin_popup_id(uint64_t id) {
    if (!popup_.open || popup_.id != id) return 0;
    if (depth_ >= kMaxWindows) return 0;
    // 宽度需求是本帧内容现算的，不能跨帧累计：累计的话内容变窄了宽度也
    // 收不回来。
    popup_.want_w = 0.0f;

    // 宽度：给了就是给的；没给就按内容自适应（缓存 / 上一帧量到的 / 估算）。
    // 菜单的估算取 max(标题宽, 140)：第一次打开时先给个不至于把条目截断的
    // 宽度，end_popup 之后就有准数了。
    float w = popup_.w;
    if (w <= 0.0f) {
        if (popup_.measured_w > 1.0f) {
            w = popup_.measured_w;
        } else if (popup_.min_w > 1.0f) {
            w = (popup_.min_w > 140.0f) ? popup_.min_w : 140.0f;
        } else {
            w = 180.0f;
        }
        if (w < popup_.min_w) w = popup_.min_w;
    }
    float       h = popup_.h;
    if (h <= 0.0f) {
        h = (popup_.measured_h > 0.0f) ? popup_.measured_h
                                       : (frame_height() * 2.0f + pad_y() * 2.0f);
    }
    Rect r{popup_.x, popup_.y, w, h};
    // 自适应高度时贴着屏幕底边往上收
    if (display_h_ > 0.0f && r.bottom() > display_h_) {
        r.y = (display_h_ - h > 0.0f) ? (display_h_ - h) : 0.0f;
    }
    // 水平方向也要留在屏幕内。弹层锚在触发它的控件下沿，而那个控件可能贴着
    // 窗口右缘（检视器里的"..."按钮就在最右）：不夹的话弹层有一半跑到屏幕
    // 外，内容被裁掉，看起来像"列表是空的"。
    if (display_w_ > 0.0f && r.right() > display_w_) {
        r.x = (display_w_ - w > 0.0f) ? (display_w_ - w) : 0.0f;
    }

    if (popup_.modal) {
        // 遮罩铺满屏幕（layer 1，先于弹层提交 → 画在弹层下面）
        const float dw = display_w_ > 0.0f ? display_w_ : 1920.0f;
        const float dh = display_h_ > 0.0f ? display_h_ : 1080.0f;
        const Rect mask{0.0f, 0.0f, dw, dh};
        Widget* m = tree_.touch(id ^ 0x3A3Aull, WidgetKind::Frame, mask, nullptr);
        if (m) {
            m->scope = id;
            m->layer = 1;
            m->corner_radius = 0.0f;   // 铺满屏幕：不能圆，否则四角漏出底图
            m->bg_overridden = true;
            m->bg_color = (theme_ && THEME_COLOR_MODAL_WINDOW_DIM_BG < THEME_COLOR_COUNT)
                        ? theme_->colors[THEME_COLOR_MODAL_WINDOW_DIM_BG] : 0x00000080u;
        }
    }

    WindowState* win = push_state(id, r, kWinTitleBar | kWinNoTitleBar | kWinNoScrollbar,
                                  true, false);
    if (!win) return 0;
    win->content_top = r.y + pad_y();
    win->cursor_y    = win->content_top;
    win->title_h     = 0.0f;

    // 背景板（不受内容裁剪约束，先提交再设裁剪）
    Widget* panel = tree_.touch(id, WidgetKind::Panel, r, nullptr);
    if (panel) {
        panel->scope = 0;
        panel->layer = 1;
        panel->shadow = true;   // 弹层浮在内容之上，投影是"层级"的唯一线索
        panel->bg_overridden = true;
        panel->bg_color = (theme_ && THEME_COLOR_POPUP_BG < THEME_COLOR_COUNT)
                        ? theme_->colors[THEME_COLOR_POPUP_BG] : 0xFF2A2A2Au;
    }
    const Rect inner = popup_clip(r, pad_x());
    tree_.set_clip(inner);

    const UiInputFrame& in = tree_.input();
    if (rect_contains(r, in.mouse_x, in.mouse_y)) mouse_in_popup_ = true;
    mouse_in_window_ = mouse_in_window_ || mouse_in_popup_;

    popup_begun_ = true;
    return 1;
}

int BuiltinUi::begin_popup(const char* id) {
    return begin_popup_id(hash_label(id));
}

int BuiltinUi::begin_modal(const char* name, float w, float h) {
    // 模态就是"点了外面不关的弹层"，位置由 open_modal 算好（居中）。这里
    // 不再要求 usable()：模态通常在所有窗口都 End 之后才开（depth 0）。
    (void)w; (void)h;
    if (!name) return 0;
    const uint64_t id = hash_label(name);
    if (!popup_.open || popup_.id != id) return 0;
    return begin_popup_id(id);
}

void BuiltinUi::end_popup() {
    if (!popup_begun_ || depth_ <= 0) return;
    WindowState& win = windows_[depth_ - 1];
    if (popup_.h <= 0.0f) {
        const float measured = win.content_h + pad_y() * 2.0f;
        if (measured > 1.0f) popup_.measured_h = measured;
    }
    // 宽度同理：这一帧各项报上来的需求汇总成下一帧的宽度（菜单还要进缓存）。
    if (popup_.w <= 0.0f && popup_.want_w > 1.0f) {
        popup_.measured_w = popup_.want_w;
        if (popup_.is_menu) remember_menu_width(popup_.id, popup_.want_w);
    }
    pop_state();
    popup_begun_ = false;
}

void BuiltinUi::close_current_popup() {
    popup_.open = false;
}

int BuiltinUi::is_popup_open(const char* id) const {
    const uint64_t pid = hash_label(id);
    return (popup_.open && popup_.id == pid) ? 1 : 0;
}

void BuiltinUi::set_tooltip(const char* text) {
    tooltip_visible_ = false;
    if (!text || !*text || depth_ == 0) return;

    const UiInputFrame& in = tree_.input();
    // 悬停对象变了就重新计时
    if (tooltip_rect_.x != last_rect_.x || tooltip_rect_.y != last_rect_.y ||
        tooltip_rect_.w != last_rect_.w || tooltip_rect_.h != last_rect_.h) {
        tooltip_rect_ = last_rect_;
        tooltip_since_ = in.time;
    }
    const bool ready = (in.time <= 0.0f) || (in.time - tooltip_since_ >= kTooltipDelay);
    if (!ready) return;

    const float tw = metrics_ ? metrics_->width(text, kWidgetFontSize) : 0.0f;
    Rect r{mouse_x_ + 14.0f, mouse_y_ + 20.0f, tw + 12.0f, frame_height()};
    if (display_w_ > 0.0f && r.right() > display_w_) r.x = display_w_ - r.w;
    if (display_h_ > 0.0f && r.bottom() > display_h_) r.y = mouse_y_ - r.h - 6.0f;

    // 工具提示不走光标流（不该推进布局），直接给矩形
    Widget* w = tree_.touch(hash_label("##tooltip"), WidgetKind::Tooltip, r, text);
    if (w) {
        w->scope = 0;
        w->layer = 1;
        w->shadow = true;
    }
    tooltip_visible_ = (w != nullptr);
}

// ---------------------------------------------------------------------------
// 拖放
// ---------------------------------------------------------------------------

int BuiltinUi::begin_drag_drop_source() {
    if (last_id_ == 0) return 0;
    if (tree_.active_id() != last_id_) return 0;
    const float dx = std::fabs(mouse_x_ - press_x_);
    const float dy = std::fabs(mouse_y_ - press_y_);
    return (dx + dy >= kDragThreshold) ? 1 : 0;
}

void BuiltinUi::set_drag_drop_payload_string(const char* type, const char* text) {
    tree_.begin_drag(last_id_, type, text);
}

void BuiltinUi::end_drag_drop_source() {
    // 松手由树在 end_frame 里清理（拖拽要跨帧存活，这里不能结束）
}

int BuiltinUi::begin_drag_drop_target() {
    if (!tree_.drag_active() || last_id_ == 0) return 0;
    return last_hovered_ ? 1 : 0;
}

int BuiltinUi::accept_drag_drop_payload_string(const char* type, char* out, int cap) {
    if (!tree_.drag_active()) return 0;
    if (!last_hovered_) return 0;
    const char* want = type ? type : "";
    if (std::strcmp(tree_.drag_payload_type(), want) != 0) return 0;
    if (!tree_.input().mouse_released[0]) return 0;

    if (out && cap > 0) {
        std::snprintf(out, static_cast<size_t>(cap), "%s", tree_.drag_payload_text());
    }
    tree_.end_drag();
    return 1;
}

void BuiltinUi::end_drag_drop_target() {
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
    // 单行文本的行高 = 字号（ImGui 的 DC.CursorPos.y 一行正好走 FontSize，
    // 行距由 ItemSpacing 单独给）。度量器的 1.25 倍行高只用于多行正文的
    // 排版估算，不能拿来当控件行高——那会让每一行比 ImGui 高出 3.5px。
    return kWidgetFontSize;
}

float BuiltinUi::frame_height() const {
    // 控件框高 = 字号 + 2×纵向内边距。内边距用 PAD_Y-1：这与 ImGui 的
    // FramePadding（= PAD_Y-1）同源，14px 字号下正好 22px。
    const float pady = (pad_y() > 1.0f) ? (pad_y() - 1.0f) : pad_y();
    return kWidgetFontSize + pady * 2.0f;
}

float BuiltinUi::item_width() {
    if (!usable()) return 0.0f;
    const float cw = content_width();
    // SetNextItemWidth：ImGui 的语义是"下一个 item 的宽度"，>0 绝对、<0 右边
    // 留白、一次性的。宿主用它把控件挤到标签左边、或给下拉框定死宽度；
    // 不支持的话输入框会一直吃 65%，旁边的按钮被推到面板外面。
    if (next_item_width_ != 0.0f) {
        const float req = next_item_width_;
        next_item_width_ = 0.0f;
        float w = (req > 0.0f) ? req : (cw + req);
        if (w < 0.0f) w = 0.0f;
        if (w > cw)   w = cw;
        return w;
    }
    // ImGui 的 ItemWidthDefault = trunc(Size.x * 0.65f)，Size.x 是子窗口的整宽
    // （含它自己的 1px 边框）。这里不能用"扣掉边框"的宽度：388 宽的子窗口在
    // ImGui 里算出来是 252，扣掉 2px 只有 250，整列控件窄 2px、右侧标签跟着
    // 左移，与 ImGui 轨就再也对不齐了。
    const Rect& r = windows_[depth_ - 1].rect;
    float w = std::floor(r.w * 0.65f);
    if (w > cw) w = cw;
    return (w > 0.0f) ? w : 0.0f;
}

float BuiltinUi::content_width() const {
    if (!usable()) return 0.0f;
    const Rect& r = windows_[depth_ - 1].rect;
    return (r.w > pad_x() * 2.0f) ? (r.w - pad_x() * 2.0f) : 0.0f;
}

float BuiltinUi::content_rect_h() const {
    if (!usable()) return 0.0f;
    const WindowState& win = windows_[depth_ - 1];
    const float h = (win.rect.bottom() - pad_y()) - win.content_top;
    return (h > 0.0f) ? h : 0.0f;
}

void BuiltinUi::content_avail(float* out_w, float* out_h) const {
    if (!usable()) {
        if (out_w) *out_w = 0.0f;
        if (out_h) *out_h = 0.0f;
        return;
    }
    // "剩余"= 内容区右下角 - 当前光标。光标含滚动位移（cursor_y 已减 scroll_y），
    // 与 ImGui 的 ContentRegionAvail 一致：滚下去之后可用高度变大。
    const WindowState& win = windows_[depth_ - 1];
    const Rect& r = win.rect;
    const float left  = r.x + pad_x() + win.indent_x;
    const float right = r.right() - pad_x();
    const float w = right - left;
    const float h = (r.bottom() - pad_y()) - win.cursor_y;
    if (out_w) *out_w = (w > 0.0f) ? w : 0.0f;
    if (out_h) *out_h = (h > 0.0f) ? h : 0.0f;
}

// ---------------------------------------------------------------------------
// 后端纹理采样约定（见 ui_texture_v_flip.h）
// ---------------------------------------------------------------------------

void set_texture_v_flip(bool flip) {
    // 只在 GRender_Init 里写一次：UI 回调路径持 g_renderer.mutex，不能反查渲染器。
    g_texture_v_flip = flip;
}

bool texture_v_flip() {
    return g_texture_v_flip;
}

} // namespace gryce_ui
