#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// BoxCollider — 盒状碰撞体占位组件。
// ---------------------------------------------------------------------------
class BoxCollider : public Component {
public:
    math::Vector3f size = math::Vector3f::one();
    math::Vector3f center = math::Vector3f::zero();
    bool is_trigger = false;

    BoxCollider() = default;

    const char* type() const override { return "BoxCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y, size.z };
        out["center"] = { center.x, center.y, center.z };
        out["is_trigger"] = is_trigger;
    }

    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{1, 1, 1});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
        is_trigger = in.value("is_trigger", false);
    }
};

} // namespace gryce_engine::components
