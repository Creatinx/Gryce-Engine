#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// SphereCollider — 球状碰撞体占位组件。
// ---------------------------------------------------------------------------
class SphereCollider : public Component {
public:
    float radius = 0.5f;
    math::Vector3f center = math::Vector3f::zero();
    bool is_trigger = false;

    SphereCollider() = default;

    const char* type() const override { return "SphereCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["center"] = { center.x, center.y, center.z };
        out["is_trigger"] = is_trigger;
    }

    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
        is_trigger = in.value("is_trigger", false);
    }
};

} // namespace gryce_engine::components
