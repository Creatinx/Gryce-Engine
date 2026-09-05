#include "GryceEngineUtils/ui/slider.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Slider::Slider(const char* id) : Widget(id) {
    style_.has_background = false;
}

void Slider::set_range(float min, float max) {
    if (max <= min) max = min + 1.0f;
    min_ = min;
    max_ = max;
    value_ = std::clamp(value_, min_, max_);
    // 不发射信号——构造函数中调用时对象尚未完全初始化
}

void Slider::set_value(float val) {
    value_ = std::clamp(val, min_, max_);
    on_value_changed.emit(normalized());
}

float Slider::normalized() const {
    return (max_ > min_) ? (value_ - min_) / (max_ - min_) : 0.0f;
}

void Slider::set_normalized(float t) {
    set_value(min_ + std::clamp(t, 0.0f, 1.0f) * (max_ - min_));
}

void Slider::drag_to(float screen_x) {
    if (!enabled_ || bounds_.w <= 0.0f) return;
    const float t = (screen_x - bounds_.x) / bounds_.w;
    set_normalized(t);
}

void Slider::drag_to_vertical(float screen_y) {
    if (!enabled_ || bounds_.h <= 0.0f) return;
    const float t = 1.0f - (screen_y - bounds_.y) / bounds_.h;
    set_normalized(t);
}

void Slider::draw(Renderer* renderer) {
    if (!visible_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    float alpha = opacity_ * s.opacity;
    if (!enabled_) alpha *= 0.4f;

    if (vertical_) {
        // 垂直滑块
        const float track_w = std::max(4.0f, bounds_.w * 0.25f);
        const float track_x = bounds_.x + (bounds_.w - track_w) * 0.5f;
        // 轨道
        r2d->draw_rect(track_x, bounds_.y, track_w, bounds_.h,
                       detail::with_alpha(Color(0.12f, 0.12f, 0.14f, 1.0f), alpha));
        // 已填充部分（从底部向上）
        const float fill_h = bounds_.h * normalized();
        if (fill_h > 0.5f) {
            r2d->draw_rect(track_x, bounds_.y + bounds_.h - fill_h, track_w, fill_h,
                           detail::with_alpha(s.background_hover, alpha));
        }
        // 手柄
        const float handle = std::max(14.0f, bounds_.w * 0.7f);
        const float hx = bounds_.x + (bounds_.w - handle) * 0.5f;
        const float hy = bounds_.y + (bounds_.h - handle) * (1.0f - normalized());
        const Color handle_color = detail::widget_background(s, state_hovered_, state_pressed_);
        detail::draw_rounded_rect(r2d, hx, hy, handle, handle,
                                  handle * 0.5f, detail::with_alpha(handle_color, alpha));
        // 焦点环
        if (state_focused_ && enabled_) {
            detail::draw_focus_ring(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                    handle * 0.5f,
                                    detail::with_alpha(Color(0.4f, 0.6f, 1.0f, 0.8f), alpha));
        }
    } else {
        const float track_h = std::max(4.0f, bounds_.h * 0.25f);
        const float track_y = bounds_.y + (bounds_.h - track_h) * 0.5f;

        // 轨道
        r2d->draw_rect(bounds_.x, track_y, bounds_.w, track_h,
                       detail::with_alpha(Color(0.12f, 0.12f, 0.14f, 1.0f), alpha));
        // 已填充部分
        const float fill_w = bounds_.w * normalized();
        if (fill_w > 0.5f) {
            r2d->draw_rect(bounds_.x, track_y, fill_w, track_h,
                           detail::with_alpha(s.background_hover, alpha));
        }
        // 手柄
        const float handle = std::max(14.0f, bounds_.h * 0.7f);
        const float hx = bounds_.x + (bounds_.w - handle) * normalized();
        const float hy = bounds_.y + (bounds_.h - handle) * 0.5f;
        const Color handle_color = detail::widget_background(s, state_hovered_, state_pressed_);
        detail::draw_rounded_rect(r2d, hx, hy, handle, handle,
                                  handle * 0.5f, detail::with_alpha(handle_color, alpha));

        // 焦点环
        if (state_focused_ && enabled_) {
            detail::draw_focus_ring(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                    handle * 0.5f,
                                    detail::with_alpha(Color(0.4f, 0.6f, 1.0f, 0.8f), alpha));
        }
    }
}

float Slider::preferred_height(float width) const {
    (void)width;
    return std::max(20.0f, style_.font_size * 1.4f);
}

bool Slider::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "min") == 0) {
        min_ = static_cast<float>(std::atof(value));
        return true;
    }
    if (std::strcmp(key, "max") == 0) {
        max_ = static_cast<float>(std::atof(value));
        return true;
    }
    if (std::strcmp(key, "value") == 0) {
        set_value(static_cast<float>(std::atof(value)));
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
