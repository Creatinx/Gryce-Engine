#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// CircleCollider2D — 2D 圆形碰撞体
// radius 为局部空间半径；世界半径 = radius * max(scale.x, scale.y)
// ---------------------------------------------------------------------------
class CircleCollider2D : public Component {
public:
    float radius = 0.5f;
    math::Vector2f center = math::Vector2f::zero();
    bool is_trigger = false;

    CircleCollider2D() = default;

    const char* type() const override { return "CircleCollider2D"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["center"] = { center.x, center.y };
        out["is_trigger"] = is_trigger;
    }

    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        auto c = in.value("center", std::vector<float>{0, 0});
        if (c.size() >= 2) center = math::Vector2f(c[0], c[1]);
        is_trigger = in.value("is_trigger", false);
    }
};

} // namespace gryce_engine::components
