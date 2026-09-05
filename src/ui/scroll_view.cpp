#include "GryceEngineUtils/ui/scroll_view.h"

#include <algorithm>

#include "GryceEngineUtils/ui/panel.h"
#include "ui_draw.h"

namespace GryceEngineUtils::ui {

ScrollView::ScrollView(const char* id) : Widget(id) {
    style_.has_background = true;
    style_.background = Color(0.10f, 0.11f, 0.13f, 1.0f);
}

void ScrollView::set_content(Widget* content) {
    if (content_ == content) return;
    if (content_ && content_->parent() == this) {
        remove_child(content_);
    }
    content_ = content;
    if (content_) {
        add_child(content_);
    }
    layout_dirty_ = true;
}

void ScrollView::scroll_to(float x, float y) {
    scroll_x_ = std::max(0.0f, x);
    scroll_y_ = std::max(0.0f, y);
}

void ScrollView::set_scroll(float x, float y) {
    scroll_to(x, y);
}

void ScrollView::start_drag(float screen_x, float screen_y) {
    if (!enabled_) return;
    dragging_ = true;
    drag_last_y_ = screen_y;
}

void ScrollView::drag_to(float screen_x, float screen_y) {
    (void)screen_x;
    if (!enabled_ || !dragging_) return;
    const float dy = screen_y - drag_last_y_;
    drag_last_y_ = screen_y;
    scroll_y_ = std::max(0.0f, scroll_y_ - dy);
    layout_dirty_ = true;
}

bool ScrollView::hit_test(float x, float y) const {
    return visible_ && bounds_.contains(x, y);
}

void ScrollView::draw(Renderer* renderer) {
    if (!visible_) return;
    Widget::draw(renderer); // 背景

    // 内容视口变换
    if (content_) {
        const float content_w = content_->size_w() > 0.0f ? content_->size_w() : bounds_.w;
        const float content_h = content_->size_h() > 0.0f
                                    ? content_->size_h()
                                    : content_->preferred_height(content_w);
        content_->set_bounds(Rect{bounds_.x - scroll_x_, bounds_.y - scroll_y_,
                                  content_w, content_h});
        if (auto* panel = dynamic_cast<Panel*>(content_)) {
            panel->relayout();
        }
    }

    // 滚动条（内容高于视口时显示）
    auto* r2d = renderer->renderer2d();
    if (!r2d || !content_) return;
    const float content_h = content_->bounds().h;
    if (content_h <= bounds_.h) return;

    // 自动隐藏：鼠标不在区域内且未拖拽时隐藏滚动条
    if (auto_hide_scrollbar_ && !state_hovered_ && !dragging_) return;

    const float alpha = opacity_ * style_.opacity;
    const float track_x = bounds_.x + bounds_.w - 8.0f;
    const float track_h = bounds_.h;
    r2d->draw_rect(track_x, bounds_.y, 6.0f, track_h,
                   detail::with_alpha(Color(0.05f, 0.05f, 0.06f, 1.0f), alpha));
    const float thumb_h = std::max(20.0f, track_h * (bounds_.h / content_h));
    const float max_scroll = std::max(0.0f, content_h - bounds_.h);
    const float thumb_y = bounds_.y + (max_scroll > 0.0f ? (scroll_y_ / max_scroll) : 0.0f) *
                                         (track_h - thumb_h);
    r2d->draw_rect(track_x, thumb_y, 6.0f, thumb_h,
                   detail::with_alpha(Color(0.5f, 0.5f, 0.55f, 1.0f), alpha));
}

} // namespace GryceEngineUtils::ui
