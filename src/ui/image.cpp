#include "GryceEngineUtils/ui/image.h"

#include <algorithm>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Image::Image(const char* id) : Widget(id) {}

void Image::set_texture(ITexture* tex) {
    texture_ = tex;
}

void Image::set_image_scale(ImageScale scale) {
    image_scale_ = scale;
}

void Image::set_tint(const Color& color) {
    tint_ = color;
}

void Image::draw(Renderer* renderer) {
    if (!visible_ || !texture_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const auto handle = renderer->texture_handle(texture_);
    if (!handle.is_valid()) return;

    float dx = bounds_.x;
    float dy = bounds_.y;
    float dw = bounds_.w;
    float dh = bounds_.h;

    if (image_scale_ == ImageScale::Fit || image_scale_ == ImageScale::Fill) {
        if (auto* tex = r2d->resolve_texture(handle)) {
            const float tw = static_cast<float>(tex->width());
            const float th = static_cast<float>(tex->height());
            if (tw > 0.0f && th > 0.0f) {
                const float scale_w = dw / tw;
                const float scale_h = dh / th;
                const float s = (image_scale_ == ImageScale::Fit)
                                    ? std::min(scale_w, scale_h)
                                    : std::max(scale_w, scale_h);
                dw = tw * s;
                dh = th * s;
                dx = bounds_.x + (bounds_.w - dw) * 0.5f;
                dy = bounds_.y + (bounds_.h - dh) * 0.5f;
            }
        }
    }

    r2d->draw_sprite(dx, dy, dw, dh, handle,
                     detail::with_alpha(tint_, opacity_ * style_.opacity));
}

bool Image::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "src") == 0) {
        // src 属性存储在 style_id 中，实际纹理加载由 UIManager 延迟处理
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
