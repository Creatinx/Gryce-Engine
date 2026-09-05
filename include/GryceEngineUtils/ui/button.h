#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class Button : public Widget {
public:
    Button(const char* id, const char* text);

    Signal<void()> on_pressed;
    Signal<void()> on_released;

    bool is_pressed() const { return pressed(); }
    bool is_hovered() const { return hovered(); }

    void set_text(const char* text);
    const std::string& text() const { return text_; }
    void set_background(const Color& normal, const Color& hover, const Color& pressed);
    void set_border_radius(float r);

    void draw(Renderer* renderer) override;
    void update(float dt) override;
    const char* type_name() const override { return "Button"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

private:
    std::string text_;
};

} // namespace GryceEngineUtils::ui
