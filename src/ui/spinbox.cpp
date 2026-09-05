#include "GryceEngineUtils/ui/spinbox.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace GryceEngineUtils::ui {

SpinBox::SpinBox(const char* id) : TextInput(id) {
    up_btn_ = new Button("spin_up", "+");
    up_btn_->set_style_class("spin-btn");
    up_btn_->style().background = Color(0.16f, 0.17f, 0.21f, 1.0f);
    up_btn_->style().background_hover = Color(0.26f, 0.32f, 0.42f, 1.0f);
    up_btn_->style().background_pressed = Color(0.10f, 0.11f, 0.14f, 1.0f);
    up_btn_->style().padding = Padding{0.0f, 0.0f, 0.0f, 0.0f};
    up_btn_->set_size(22.0f, 0.0f);
    up_btn_->on_pressed.connect([this]() { step_value(step_); });

    down_btn_ = new Button("spin_down", "-");
    down_btn_->set_style_class("spin-btn");
    down_btn_->style().background = Color(0.16f, 0.17f, 0.21f, 1.0f);
    down_btn_->style().background_hover = Color(0.26f, 0.32f, 0.42f, 1.0f);
    down_btn_->style().background_pressed = Color(0.10f, 0.11f, 0.14f, 1.0f);
    down_btn_->style().padding = Padding{0.0f, 0.0f, 0.0f, 0.0f};
    down_btn_->set_size(22.0f, 0.0f);
    down_btn_->on_pressed.connect([this]() { step_value(-step_); });

    add_child(up_btn_);
    add_child(down_btn_);

    on_submit.connect([this]() { apply_text(); });
    set_text("0");
    apply_text();
}

void SpinBox::set_range(float min, float max) {
    if (max < min) max = min;
    min_ = min;
    max_ = max;
    set_value(value_);
}

void SpinBox::set_step(float step) {
    step_ = std::max(0.0001f, step);
}

void SpinBox::set_value(float value) {
    value_ = std::clamp(value, min_, max_);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value_);
    set_text(buf);
    on_value_changed.emit(value_);
}

void SpinBox::step_value(float delta) {
    set_value(value_ + delta);
}

void SpinBox::apply_text() {
    if (!enabled_) return;
    char* end = nullptr;
    const float v = std::strtof(text(), &end);
    if (end && end != text()) {
        set_value(v);
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", value_);
        set_text(buf);
    }
}

void SpinBox::update(float dt) {
    TextInput::update(dt);
    // 定位上下按钮
    const float bw = 22.0f;
    const float bh = std::max(1.0f, bounds_.h * 0.5f);
    if (up_btn_) {
        up_btn_->set_bounds(Rect{bounds_.x + bounds_.w - bw, bounds_.y, bw, bh});
    }
    if (down_btn_) {
        down_btn_->set_bounds(Rect{bounds_.x + bounds_.w - bw, bounds_.y + bh, bw, bh});
    }
}

void SpinBox::draw(Renderer* renderer) {
    TextInput::draw(renderer);
}

float SpinBox::preferred_height(float width) const {
    (void)width;
    return std::max(32.0f, TextInput::preferred_height(width));
}

} // namespace GryceEngineUtils::ui
