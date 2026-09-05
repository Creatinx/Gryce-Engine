#pragma once

#include <string>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class TextInput : public Widget {
public:
    explicit TextInput(const char* id);

    Signal<void(const char*)> on_text_changed;
    Signal<void()> on_submit;

    void set_placeholder(const char* text);
    void set_max_length(int len);
    void set_text(const char* text);
    const char* text() const { return text_.c_str(); }
    void set_cursor(int index);
    int cursor() const { return cursor_; }

    void draw(Renderer* renderer) override;
    void update(float dt) override;
    const char* type_name() const override { return "TextInput"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

    // 供 UIManager 输入转发
    void handle_text_input(const char* utf8);
    void handle_key(int key, bool down);
    void set_cursor_from_screen_x(float x);

private:
    std::string text_;
    std::string placeholder_;
    int max_length_ = 0; // 0 = 不限
    int cursor_ = 0;
    float blink_timer_ = 0.0f;
    bool show_cursor_ = true;
};

} // namespace GryceEngineUtils::ui
