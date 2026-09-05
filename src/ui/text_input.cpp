#include "GryceEngineUtils/ui/text_input.h"

#include <algorithm>
#include <cstring>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

namespace {

// GLFW 键码（UIManager 转发自 Renderer，解释仅限文本编辑所需按键）
constexpr int kKeyEnter = 257;
constexpr int kKeyBackspace = 259;
constexpr int kKeyDelete = 261;
constexpr int kKeyLeft = 263;
constexpr int kKeyRight = 262;
constexpr int kKeyHome = 268;
constexpr int kKeyEnd = 269;

} // namespace

TextInput::TextInput(const char* id) : Widget(id) {
    style_.has_background = true;
    style_.background = Color(0.08f, 0.08f, 0.10f, 1.0f);
    style_.background_hover = Color(0.10f, 0.10f, 0.13f, 1.0f);
    style_.background_pressed = style_.background_hover;
}

void TextInput::set_placeholder(const char* text) {
    placeholder_ = text ? text : "";
}

void TextInput::set_max_length(int len) {
    max_length_ = std::max(0, len);
    if (max_length_ > 0 && static_cast<int>(text_.size()) > max_length_) {
        text_.resize(static_cast<size_t>(max_length_));
        cursor_ = std::min(cursor_, static_cast<int>(text_.size()));
    }
}

void TextInput::set_text(const char* text) {
    text_ = text ? text : "";
    if (max_length_ > 0 && static_cast<int>(text_.size()) > max_length_) {
        text_.resize(static_cast<size_t>(max_length_));
    }
    cursor_ = static_cast<int>(text_.size());
    on_text_changed.emit(text_.c_str());
}

void TextInput::update(float dt) {
    blink_timer_ += dt;
    if (blink_timer_ >= 0.5f) {
        blink_timer_ = 0.0f;
        show_cursor_ = !show_cursor_;
    }
}

void TextInput::set_cursor(int index) {
    cursor_ = std::clamp(index, 0, static_cast<int>(text_.size()));
}

void TextInput::handle_text_input(const char* utf8) {
    if (!utf8 || !*utf8 || !enabled_) return;
    const int count = static_cast<int>(text_.size()) + static_cast<int>(std::strlen(utf8));
    if (max_length_ > 0 && count > max_length_) return;
    text_.insert(static_cast<size_t>(cursor_), utf8);
    cursor_ += static_cast<int>(std::strlen(utf8));
    on_text_changed.emit(text_.c_str());
}

void TextInput::handle_key(int key, bool down) {
    if (!down || !enabled_) return;
    const int len = static_cast<int>(text_.size());
    if (key == kKeyEnter) {
        on_submit.emit();
    } else if (key == kKeyBackspace && cursor_ > 0) {
        text_.erase(static_cast<size_t>(cursor_ - 1), 1);
        --cursor_;
        on_text_changed.emit(text_.c_str());
    } else if (key == kKeyDelete && cursor_ < len) {
        text_.erase(static_cast<size_t>(cursor_), 1);
        on_text_changed.emit(text_.c_str());
    } else if (key == kKeyLeft) {
        cursor_ = std::max(0, cursor_ - 1);
    } else if (key == kKeyRight) {
        cursor_ = std::min(len, cursor_ + 1);
    } else if (key == kKeyHome) {
        cursor_ = 0;
    } else if (key == kKeyEnd) {
        cursor_ = len;
    }
}

void TextInput::set_cursor_from_screen_x(float x) {
    if (!enabled_) return;
    const float font_size = style_.font_size;
    const float text_x = bounds_.x + padding_.left;
    int best = static_cast<int>(text_.size());
    for (size_t i = 0; i <= text_.size(); ++i) {
        const std::string prefix = text_.substr(0, i);
        const float w = detail::text_width(prefix, font_size);
        if (text_x + w >= x) {
            best = static_cast<int>(i);
            break;
        }
    }
    cursor_ = best;
}

void TextInput::draw(Renderer* renderer) {
    if (!visible_) return;
    Widget::draw(renderer); // 背景 + 边框
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    const float alpha = opacity_ * s.opacity;
    const float font_size = s.font_size;
    const float text_x = bounds_.x + padding_.left;
    const float text_y = bounds_.y + (bounds_.h - font_size) * 0.5f;
    const float max_w = std::max(0.0f, bounds_.w - padding_.left - padding_.right);

    if (text_.empty() && !placeholder_.empty()) {
        r2d->draw_text(text_x, text_y,
                       detail::truncate_text(placeholder_, font_size, max_w),
                       font_size, detail::with_alpha(Color(0.45f, 0.45f, 0.48f, 1.0f), alpha));
    } else {
        r2d->draw_text(text_x, text_y,
                       detail::truncate_text(text_, font_size, max_w),
                       font_size, detail::with_alpha(s.color, alpha));
    }

    // 光标（聚焦时闪烁）
    if (state_focused_) {
        if (show_cursor_) {
            const float cx = text_x + detail::text_width(
                text_.substr(0, static_cast<size_t>(cursor_)), font_size);
            r2d->draw_rect(cx, text_y, std::max(1.5f, font_size * 0.08f), font_size,
                           detail::with_alpha(s.color, alpha));
        }
    }
}

float TextInput::preferred_height(float width) const {
    (void)width;
    return std::max(30.0f, style_.font_size * 1.8f + padding_.top + padding_.bottom);
}

bool TextInput::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "text") == 0) {
        set_text(value);
        return true;
    }
    if (std::strcmp(key, "placeholder") == 0) {
        set_placeholder(value);
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
