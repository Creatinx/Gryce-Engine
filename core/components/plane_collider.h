#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// PlaneCollider — 无限平面碰撞体，适合地面/墙面。
// normal 指向碰撞体“外部”，即动态物体可以存在的半空间。
// ---------------------------------------------------------------------------
class PlaneCollider : public Component {
public:
    math::Vector3f normal = math::Vector3f::up(); // 默认朝上（地面）
    float offset = 0.0f;                           // 沿法向的平面偏移
    bool is_trigger = false;

    PlaneCollider() = default;

    const char* type() const override { return "PlaneCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["normal"] = { normal.x, normal.y, normal.z };
        out["offset"] = offset;
        out["is_trigger"] = is_trigger;
    }

    void deserialize(const nlohmann::json& in) override {
        auto n = in.value("normal", std::vector<float>{0, 1, 0});
        if (n.size() >= 3) normal = math::Vector3f(n[0], n[1], n[2]).normalized();
        offset = in.value("offset", 0.0f);
        is_trigger = in.value("is_trigger", false);
    }
};

} // namespace gryce_engine::components
