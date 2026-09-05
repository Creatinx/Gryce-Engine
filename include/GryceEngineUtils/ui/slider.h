#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class Slider : public Widget {
public:
    explicit Slider(const char* id);

    Signal<void(float)> on_value_changed; // 0~1

    void set_range(float min, float max);
    float min_value() const { return min_; }
    float max_value() const { return max_; }
    void set_value(float val);
    float value() const { return value_; }
    float normalized() const;
    void set_normalized(float t);
    // 供 UIManager 拖拽：把屏幕坐标映射为值
    void drag_to(float screen_x);
    void drag_to_vertical(float screen_y);

    // 垂直方向
    void set_vertical(bool vert) { vertical_ = vert; }
    bool is_vertical() const { return vertical_; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "Slider"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

private:
    float min_ = 0.0f;
    float max_ = 1.0f;
    float value_ = 0.5f;
    bool dragging_ = false;
    bool vertical_ = false;
};

} // namespace GryceEngineUtils::ui
