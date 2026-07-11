#pragma once

#include "components/2d/component_2d.h"

namespace gryce_engine::components::d2::light {

// ---------------------------------------------------------------------------
// Light2D — 2D 点光源组件
// 挂载到 Entity 上，使用 owner 的 Transform.position 作为光源位置。
// ---------------------------------------------------------------------------
class Light2D : public Component2D {
public:
    enum class LightType {
        Point,
        Directional
    };

    LightType light_type = LightType::Point;
    render::Color color = render::Color::white();
    float intensity = 1.0f;
    float radius = 200.0f;      // 点光源影响半径（屏幕空间像素）
    math::Vector2f direction;   // 方向光方向（仅 Directional 有效）

    Light2D() = default;
    Light2D(const render::Color& c, float intens, float rad)
        : color(c), intensity(intens), radius(rad) {}

    const char* type() const override { return "Light2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["light_type"] = (light_type == LightType::Point) ? "point" : "directional";
        out["color"] = { color.r, color.g, color.b, color.a };
        out["intensity"] = intensity;
        out["radius"] = radius;
        out["direction"] = { direction.x, direction.y };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        std::string lt = in.value("light_type", "point");
        light_type = (lt == "directional") ? LightType::Directional : LightType::Point;
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        intensity = in.value("intensity", 1.0f);
        radius = in.value("radius", 200.0f);
        auto d = in.value("direction", std::vector<float>{0, -1});
        if (d.size() >= 2) direction = math::Vector2f(d[0], d[1]);
    }

    void draw(render::IRenderer2D* /*renderer*/) override {
        // Light2D 本身不直接绘制，由渲染系统收集后统一处理。
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, static_cast<uint64_t>(light_type));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        hash_combine(h, hash_float(intensity));
        hash_combine(h, hash_float(radius));
        hash_combine(h, hash_float(direction.x));
        hash_combine(h, hash_float(direction.y));
        return h;
    }
};

} // namespace gryce_engine::components::d2::light
