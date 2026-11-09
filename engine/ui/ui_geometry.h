#pragma once

#include <algorithm>

namespace gryce_engine::ui {

// UI 统一使用左上原点、+Y 向下、单位逻辑像素的屏幕矩形。
struct UIRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return w <= 0.0f || h <= 0.0f; }

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }

    UIRect inset(float amount) const {
        return {x + amount, y + amount,
                std::max(0.0f, w - amount * 2.0f),
                std::max(0.0f, h - amount * 2.0f)};
    }

    UIRect intersect(const UIRect& other) const {
        const float left = std::max(x, other.x);
        const float top = std::max(y, other.y);
        const float r = std::min(right(), other.right());
        const float b = std::min(bottom(), other.bottom());
        return {left, top, std::max(0.0f, r - left), std::max(0.0f, b - top)};
    }
};

} // namespace gryce_engine::ui
