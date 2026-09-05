#include "GryceEngineUtils/ui/progress_bar.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

ProgressBar::ProgressBar(const char* id) : Widget(id) {
    style_.has_background = false;
}

void ProgressBar::set_progress(float p) {
    progress_ = std::clamp(p, 0.0f, 1.0f);
}

void ProgressBar::set_color(const Color& fill, const Color& bg) {
    fill_color_ = fill;
    bg_color_ = bg;
}

void ProgressBar::set_show_text(bool show) {
    show_text_ = show;
}

void ProgressBar::update(float dt) {
    Widget::update(dt);
    if (indeterminate_) {
        anim_offset_ += dt * 120.0f; // 每秒移动 120 像素
        if (anim_offset_ > bounds_.w * 2.0f) anim_offset_ = 0.0f;
    }
}

void ProgressBar::draw(Renderer* renderer) {
    if (!visible_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    float alpha = opacity_ * style_.opacity;
    if (!enabled_) alpha *= 0.4f;
    // 背景
    detail::draw_rounded_rect(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                              style_.border_radius, detail::with_alpha(bg_color_, alpha));

    if (indeterminate_) {
        // 不确定模式：移动的条纹块
        const float stripe_w = bounds_.w * 0.3f;
        const float sx = bounds_.x + anim_offset_ - stripe_w;
        detail::draw_rounded_rect(r2d, sx, bounds_.y, stripe_w, bounds_.h,
                                  style_.border_radius, detail::with_alpha(fill_color_, alpha * 0.7f));
    } else {
        // 确定模式：按进度填充
        const float fill_w = bounds_.w * progress_;
        if (fill_w > 1.0f) {
            detail::draw_rounded_rect(r2d, bounds_.x, bounds_.y, fill_w, bounds_.h,
                                      style_.border_radius, detail::with_alpha(fill_color_, alpha));
        }
    }

    // 百分比文字
    if (show_text_ && !indeterminate_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(progress_ * 100.0f + 0.5f));
        const std::string label(buf);
        const float tw = detail::text_width(label, 12.0f);
        r2d->draw_text(bounds_.x + (bounds_.w - tw) * 0.5f,
                       bounds_.y + (bounds_.h - 12.0f) * 0.5f,
                       label, 12.0f, detail::with_alpha(Color::white(), alpha));
    }
}

float ProgressBar::preferred_height(float width) const {
    (void)width;
    return 18.0f;
}

bool ProgressBar::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "value") == 0) {
        set_progress(static_cast<float>(std::atof(value)) / 100.0f);
        return true;
    }
    if (std::strcmp(key, "min") == 0 || std::strcmp(key, "max") == 0) {
        return true; // ProgressBar 忽略 min/max，默认 0~100
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
