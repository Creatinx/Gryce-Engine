#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// BoxCollider2D — 2D 矩形碰撞体
// size 为局部空间的完整宽高；世界空间 half-extents = size * scale * 0.5
// ---------------------------------------------------------------------------
class BoxCollider2D : public Component {
public:
    math::Vector2f size = math::Vector2f::one();
    math::Vector2f center = math::Vector2f::zero();
    bool is_trigger = false;

    BoxCollider2D() = default;

    const char* type() const override { return "BoxCollider2D"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y };
        out["center"] = { center.x, center.y };
        out["is_trigger"] = is_trigger;
    }

    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{1, 1});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        auto c = in.value("center", std::vector<float>{0, 0});
        if (c.size() >= 2) center = math::Vector2f(c[0], c[1]);
        is_trigger = in.value("is_trigger", false);
    }
};

} // namespace gryce_engine::components
