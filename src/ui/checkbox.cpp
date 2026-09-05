#include "GryceEngineUtils/ui/checkbox.h"

#include <algorithm>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

CheckBox::CheckBox(const char* id, const char* label)
    : Widget(id), label_(label ? label : "") {}

void CheckBox::set_checked(bool checked) {
    if (checked_ == checked) return;
    checked_ = checked;
    on_toggled.emit(checked_);
}

void CheckBox::toggle() {
    checked_ = !checked_;
    on_toggled.emit(checked_);
}

void CheckBox::set_label(const char* label) {
    label_ = label ? label : "";
}

void CheckBox::draw(Renderer* renderer) {
    if (!visible_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    float alpha = opacity_ * s.opacity;
    if (!enabled_) alpha *= 0.4f;
    const float box = std::min(18.0f, bounds_.h);
    const float box_x = bounds_.x;
    const float box_y = bounds_.y + (bounds_.h - box) * 0.5f;

    // 勾选框
    const Color box_color = detail::widget_background(s, state_hovered_, state_pressed_);
    detail::draw_rounded_rect(r2d, box_x, box_y, box, box,
                              std::min(3.0f, box * 0.25f), detail::with_alpha(box_color, alpha));
    if (checked_) {
        // 勾号（✓ 形填充多边形）
        const Color check_color = detail::with_alpha(s.color, alpha);
        const float lw = std::max(2.0f, box * 0.12f);
        const float hw = lw * 0.5f;
        // 多边形顶点沿勾号轮廓：(0.22,0.55) → (0.42,0.75) → (0.76,0.32)
        const float x0 = box_x + box * 0.22f, y0 = box_y + box * 0.55f;
        const float x1 = box_x + box * 0.42f, y1 = box_y + box * 0.75f;
        const float x2 = box_x + box * 0.76f, y2 = box_y + box * 0.32f;
        std::vector<math::Vector2f> pts = {
            {x0, y0 - hw}, {x1, y1 - hw}, {x2, y2 - hw},
            {x2, y2 + hw}, {x1, y1 + hw}, {x0, y0 + hw},
        };
        r2d->draw_polygon(pts, check_color);
    }

    // 标签
    const float text_x = box_x + box + 8.0f;
    const float text_w = std::max(0.0f, bounds_.w - box - 8.0f);
    const Rect label_bounds{text_x, bounds_.y, text_w, bounds_.h};
    const Padding label_pad{0.0f, 0.0f, 0.0f, 0.0f};
    detail::draw_label_text(r2d, label_bounds, s, label_, alpha, label_pad);

    // 焦点环
    if (state_focused_ && enabled_) {
        detail::draw_focus_ring(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                std::min(3.0f, box * 0.25f),
                                detail::with_alpha(Color(0.4f, 0.6f, 1.0f, 0.8f), alpha));
    }
}

float CheckBox::preferred_height(float width) const {
    (void)width;
    return std::max(24.0f, style_.font_size * 1.6f);
}

bool CheckBox::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "text") == 0) {
        set_label(value);
        return true;
    }
    if (std::strcmp(key, "checked") == 0) {
        set_checked(std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0);
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
