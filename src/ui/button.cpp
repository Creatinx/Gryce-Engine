#include "GryceEngineUtils/ui/button.h"

#include <algorithm>
#include <cstring>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Button::Button(const char* id, const char* text)
    : Widget(id), text_(text ? text : "") {
    style_.has_background = true;
}

void Button::set_text(const char* text) {
    text_ = text ? text : "";
}

void Button::set_background(const Color& normal, const Color& hover, const Color& pressed) {
    style_.background = normal;
    style_.background_hover = hover;
    style_.background_pressed = pressed;
    style_.has_background = true;
}

void Button::set_border_radius(float r) {
    style_.border_radius = std::max(0.0f, r);
}

void Button::draw(Renderer* renderer) {
    if (!visible_) return;
    Widget::draw(renderer); // 背景 + 边框 + 焦点环
    if (text_.empty()) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    float alpha = opacity_ * style_.opacity;
    if (!enabled_) alpha *= 0.4f;
    detail::draw_label_text(r2d, bounds_, style_, text_, alpha, padding_);
}

void Button::update(float dt) {
    Widget::update(dt);
}

float Button::preferred_height(float width) const {
    (void)width;
    return std::max(36.0f, style_.font_size * 1.8f + padding_.top + padding_.bottom);
}

bool Button::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "text") == 0) {
        set_text(value);
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
