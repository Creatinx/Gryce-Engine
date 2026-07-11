#pragma once

#include <string>

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Light — 光源组件。
// 目前支持 Directional / Point / Spot 三种类型（后两者为占位，渲染管线当前只使用 Directional）。
// ---------------------------------------------------------------------------
class Light : public Component {
public:
    enum class Type { Directional = 0, Point = 1, Spot = 2 };

    Type light_type = Type::Directional;
    math::Vector3f color = math::Vector3f::one();
    float intensity = 1.0f;

    // 方向光：照射方向（由 owner Transform 的旋转也可推导，但保留显式方向便于编辑）。
    math::Vector3f direction = math::Vector3f(0.0f, -1.0f, 0.0f);

    // 点光/聚光：有效范围（占位）。
    float range = 10.0f;
    // 聚光：锥角（度，占位）。
    float spot_angle = 45.0f;

    Light() = default;

    const char* type() const override { return "Light"; }

    void serialize(nlohmann::json& out) const override {
        out["light_type"] = static_cast<int>(light_type);
        out["color"] = { color.x, color.y, color.z };
        out["intensity"] = intensity;
        out["direction"] = { direction.x, direction.y, direction.z };
        out["range"] = range;
        out["spot_angle"] = spot_angle;
    }

    void deserialize(const nlohmann::json& in) override {
        light_type = static_cast<Type>(in.value("light_type", 0));
        auto c = in.value("color", std::vector<float>{1, 1, 1});
        if (c.size() >= 3) color = math::Vector3f(c[0], c[1], c[2]);
        intensity = in.value("intensity", 1.0f);
        auto d = in.value("direction", std::vector<float>{0, -1, 0});
        if (d.size() >= 3) direction = math::Vector3f(d[0], d[1], d[2]);
        range = in.value("range", 10.0f);
        spot_angle = in.value("spot_angle", 45.0f);
    }
};

} // namespace gryce_engine::components
