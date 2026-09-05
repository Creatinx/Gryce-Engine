#include "GryceEngineUtils/ui/divider.h"

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Divider::Divider(const char* id) : Widget(id) {}

void Divider::draw(Renderer* renderer) {
    if (!visible_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;
    const float alpha = opacity_ * style_.opacity;
    const Color c = detail::with_alpha(color_, alpha);
    if (vertical_) {
        const float x = bounds_.x + bounds_.w * 0.5f - 1.0f;
        r2d->draw_rect(x, bounds_.y + 2.0f, 2.0f, std::max(1.0f, bounds_.h - 4.0f), c);
    } else {
        const float y = bounds_.y + bounds_.h * 0.5f - 1.0f;
        r2d->draw_rect(bounds_.x + 2.0f, y, std::max(1.0f, bounds_.w - 4.0f), 2.0f, c);
    }
}

} // namespace GryceEngineUtils::ui
