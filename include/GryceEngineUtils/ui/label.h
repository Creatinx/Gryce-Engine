#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class Label : public Widget {
public:
    Label(const char* id, const char* text);

    void set_text(const char* text);
    const std::string& text() const { return text_; }
    void set_font_size(float size);
    void set_color(const Color& color);
    void set_text_align(TextAlign align);

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "Label"; }
    float preferred_height(float width) const override;
    float preferred_width() const override;
    bool set_property(const char* key, const char* value) override;

private:
    std::string text_;
};

} // namespace GryceEngineUtils::ui
