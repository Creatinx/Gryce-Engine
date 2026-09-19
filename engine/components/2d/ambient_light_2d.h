#pragma once

#include "components/2d/component_2d.h"

namespace gryce_engine::components::d2::light {

// ---------------------------------------------------------------------------
// AmbientLight2D — 2D 环境光组件
// 挂载到任意 Entity 上，设置整个 2D 场景的环境光底色。
// 多个 AmbientLight2D 同时存在时，取第一个启用的组件。
// ---------------------------------------------------------------------------
class AmbientLight2D : public Component2D {
public:
    render::Color color = render::Color(0.15f, 0.15f, 0.20f, 1.0f);
    float intensity = 1.0f;

    AmbientLight2D() = default;
    explicit AmbientLight2D(const render::Color& c, float intens = 1.0f)
        : color(c), intensity(intens) {}

    const char* type() const override { return "AmbientLight2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["color"] = { color.r, color.g, color.b, color.a };
        out["intensity"] = intensity;
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        auto c = in.value("color", std::vector<float>{0.15f, 0.15f, 0.20f, 1.0f});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        intensity = in.value("intensity", 1.0f);
    }

    void draw(render::IRenderer2D* /*renderer*/) override {
        // 环境光由 RenderSystem2D 统一收集设置，组件本身不绘制。
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        hash_combine(h, hash_float(intensity));
        return h;
    }
};

} // namespace gryce_engine::components::d2::light
