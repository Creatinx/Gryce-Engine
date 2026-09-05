#include "GryceEngineUtils/ui/label.h"

#include <algorithm>
#include <cstring>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Label::Label(const char* id, const char* text)
    : Widget(id), text_(text ? text : "") {}

void Label::set_text(const char* text) {
    text_ = text ? text : "";
}

void Label::set_font_size(float size) {
    style_.font_size = std::max(1.0f, size);
}

void Label::set_color(const Color& color) {
    style_.color = color;
}

void Label::set_text_align(TextAlign align) {
    style_.text_align = align;
}

void Label::draw(Renderer* renderer) {
    if (!visible_ || text_.empty()) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    const float font_size = s.font_size;
    const float pad_l = padding_.left;
    const float pad_r = padding_.right;
    const float max_w = std::max(0.0f, bounds_.w - pad_l - pad_r);
    const std::string shown = detail::truncate_text(text_, font_size, max_w);
    const float tw = detail::text_width(shown, font_size);

    float x = bounds_.x + pad_l;
    switch (s.text_align) {
        case TextAlign::Left: break;
        case TextAlign::Center: x = bounds_.x + bounds_.w * 0.5f - tw * 0.5f; break;
        case TextAlign::Right: x = bounds_.x + bounds_.w - tw - pad_r; break;
    }
    const float y = bounds_.y + std::max(0.0f, (bounds_.h - font_size) * 0.5f);

    r2d->draw_text(x, y, shown, font_size,
                   detail::with_alpha(s.color, opacity_ * s.opacity));
}

float Label::preferred_height(float width) const {
    (void)width;
    return style_.font_size * 1.5f + padding_.top + padding_.bottom;
}

float Label::preferred_width() const {
    return detail::text_width(text_, style_.font_size) + padding_.left + padding_.right;
}

bool Label::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "text") == 0) {
        set_text(value);
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
