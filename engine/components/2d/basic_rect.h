#pragma once

#include "components/2d/component_2d.h"

namespace gryce_engine::components::d2::basic_rect {

// ---------------------------------------------------------------------------
// BasicRect — 矩形组件基类
// ---------------------------------------------------------------------------
class BasicRect : public Component2D {
public:
    float width = 0.0f;
    float height = 0.0f;

protected:
    BasicRect() = default;

    void serialize_base(nlohmann::json& out) const {
        out["width"] = width;
        out["height"] = height;
    }

    void deserialize_base(const nlohmann::json& in) {
        width = in.value("width", 0.0f);
        height = in.value("height", 0.0f);
    }
};

// ---------------------------------------------------------------------------
// ColorRect — 彩色填充矩形
// ---------------------------------------------------------------------------
class ColorRect : public BasicRect {
public:
    render::Color color;

    ColorRect() = default;
    ColorRect(float w, float h, const render::Color& c)
        : color(c) {
        width = w;
        height = h;
    }

    const char* type() const override { return "ColorRect"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        serialize_base(out);
        out["color"] = { color.r, color.g, color.b, color.a };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        deserialize_base(in);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    }

    void draw(render::IRenderer2D* renderer) override {
        if (!enabled || !renderer) return;
        math::Vector2f pos = position();
        math::Vector2f s = scale();
        renderer->draw_rect(pos.x, pos.y, width * s.x, height * s.y, color);
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_float(width));
        hash_combine(h, hash_float(height));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        return h;
    }
};

} // namespace gryce_engine::components::d2::basic_rect
