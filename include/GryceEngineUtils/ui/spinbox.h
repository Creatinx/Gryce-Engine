#pragma once

#include <string>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/text_input.h"

namespace GryceEngineUtils::ui {

// SpinBox — 数值微调框（文本 + 上下按钮）
class SpinBox : public TextInput {
public:
    explicit SpinBox(const char* id = "spinbox");

    Signal<void(float)> on_value_changed;

    void set_range(float min, float max);
    void set_step(float step);
    void set_value(float value);
    float value() const { return value_; }
    float min() const { return min_; }
    float max() const { return max_; }

    void draw(Renderer* renderer) override;
    void update(float dt) override;
    const char* type_name() const override { return "SpinBox"; }
    float preferred_height(float width) const override;

private:
    void apply_text();
    void step_value(float delta);

    float value_ = 0.0f;
    float min_ = 0.0f;
    float max_ = 100.0f;
    float step_ = 1.0f;
    Button* up_btn_ = nullptr;
    Button* down_btn_ = nullptr;
};

} // namespace GryceEngineUtils::ui
