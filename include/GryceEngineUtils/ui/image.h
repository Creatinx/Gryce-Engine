#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

enum class ImageScale {
    Fill, Fit, Stretch, None
};

class Image : public Widget {
public:
    explicit Image(const char* id);

    void set_texture(ITexture* tex);
    ITexture* texture() const { return texture_; }
    void set_image_scale(ImageScale scale);
    void set_tint(const Color& color);

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "Image"; }
    bool set_property(const char* key, const char* value) override;

private:
    ITexture* texture_ = nullptr;
    ImageScale image_scale_ = ImageScale::Stretch;
    Color tint_ = Color::white();
};

} // namespace GryceEngineUtils::ui
