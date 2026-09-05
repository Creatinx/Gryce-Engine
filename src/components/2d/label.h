#pragma once

#include <string>

#include "components/2d/component_2d.h"

namespace gryce_engine::components::d2::text {

// ---------------------------------------------------------------------------
// Label — 文字标签组件
// ---------------------------------------------------------------------------
class Label : public Component2D {
public:
    std::string text;
    float font_size = 16.0f;
    render::Color color;

    Label() { render_order = k_ui_render_order; }
    Label(const std::string& str, float size, const render::Color& c)
        : text(str), font_size(size), color(c) {
        render_order = k_ui_render_order;
    }

    static constexpr int k_ui_render_order = 1000;

    const char* type() const override { return "Label"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["text"] = text;
        out["font_size"] = font_size;
        out["color"] = { color.r, color.g, color.b, color.a };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        text = in.value("text", "");
        font_size = in.value("font_size", 16.0f);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    }

    void draw(render::IRenderer2D* renderer) override {
        if (!enabled || !renderer || text.empty()) return;
        math::Vector2f pos = position();
        renderer->draw_text(pos.x, pos.y, text, font_size * scale().x, color);
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_string(text));
        hash_combine(h, hash_float(font_size));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        return h;
    }
};

} // namespace gryce_engine::components::d2::text
