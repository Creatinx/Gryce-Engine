// ===========================================================================
// engine/ui/ui_layout.cpp — 确定性布局栈的实现
//
// 全部是浮点加减，没有任何隐式输入（不读窗口、不读 ImGui、不读时间），
// 所以同样的调用序列必然给出同样的矩形——这是它可被单测的前提。
// ===========================================================================

#include "ui/ui_layout.h"

namespace gryce_ui {

void Layout::reset(const Rect& viewport) {
    depth_ = 0;
    stack_[0].area      = viewport;
    stack_[0].row_h     = 0.0f;
    stack_[0].cursor_x  = viewport.x;
    stack_[0].cursor_y  = viewport.y;
    stack_[0].indent    = 0.0f;
    stack_[0].row_open  = false;
    depth_ = 1;
}

void Layout::begin_panel(const Rect& area, float pad) {
    if (depth_ >= kMaxDepth) return;   // 栈满：忽略（层级深度是调用方的 bug）
    Level& l = stack_[depth_];
    l.area.x = area.x + pad;
    l.area.y = area.y + pad;
    l.area.w = area.w - pad * 2.0f;
    l.area.h = area.h - pad * 2.0f;
    if (l.area.w < 0.0f) l.area.w = 0.0f;
    if (l.area.h < 0.0f) l.area.h = 0.0f;
    l.row_h    = 0.0f;
    l.cursor_x = l.area.x;
    l.cursor_y = l.area.y;
    l.indent   = 0.0f;
    l.row_open = false;
    ++depth_;
}

void Layout::end_panel() {
    if (depth_ > 1) --depth_;
}

Rect Layout::content() const { return cur().area; }

float Layout::avail_height() const {
    const float remain = cur().area.bottom() - cur().cursor_y;
    return remain > 0.0f ? remain : 0.0f;
}

float Layout::cursor_y() const { return cur().cursor_y; }

void Layout::begin_row(float height) {
    Level& l = cur();
    if (l.row_open) end_row();
    l.row_h   = height > 0.0f ? height : 0.0f;
    l.row_max_h = 0.0f;
    l.cursor_x = l.area.x + l.indent;
    l.row_open = true;
}

void Layout::end_row() {
    Level& l = cur();
    if (!l.row_open) return;
    const float advance = (l.row_max_h > l.row_h ? l.row_max_h : l.row_h) + spacing_;
    l.cursor_y += advance;
    l.row_open = false;
}

Rect Layout::cell(float w, float h) {
    Level& l = cur();
    Rect r;
    r.x = l.cursor_x;
    r.y = l.cursor_y;
    r.w = (w <= 0.0f) ? (l.area.right() - l.cursor_x) : w;
    r.h = (h <= 0.0f) ? l.row_h : h;
    if (r.w < 0.0f) r.w = 0.0f;
    if (r.h > l.row_max_h) l.row_max_h = r.h;
    l.cursor_x += r.w + spacing_;
    return r;
}

void Layout::space(float px) {
    Level& l = cur();
    if (l.row_open) end_row();
    l.cursor_y += px;
}

void Layout::indent(float px) {
    cur().indent += px;
    if (cur().indent < 0.0f) cur().indent = 0.0f;
}

void Layout::unindent(float px) {
    indent(-px);
}

void Layout::set_spacing(float px) {
    spacing_ = px >= 0.0f ? px : 0.0f;
}

float Layout::spacing() const { return spacing_; }

} // namespace gryce_ui
