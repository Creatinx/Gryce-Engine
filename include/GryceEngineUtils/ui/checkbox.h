#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class CheckBox : public Widget {
public:
    CheckBox(const char* id, const char* label);

    Signal<void(bool)> on_toggled;

    void set_checked(bool checked);
    void toggle();
    bool is_checked() const { return checked_; }
    bool checked() const { return checked_; } // 别名
    void set_label(const char* label);
    const std::string& label() const { return label_; }
    void set_text(const char* t) { set_label(t); }
    const std::string& text() const { return label_; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "CheckBox"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

private:
    bool checked_ = false;
    std::string label_;
};

} // namespace GryceEngineUtils::ui
