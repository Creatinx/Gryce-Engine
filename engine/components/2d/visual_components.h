#pragma once

#include "components/2d/component_2d.h"
#include "render/render2d.h"

#include <string>
#include <vector>

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// NinePatchRect — 九宫格图像（对话框、按钮底，任意缩放不变形）
// ---------------------------------------------------------------------------
class NinePatchRect : public Component2D {
public:
    std::string texture_path;
    float left_margin = 4.0f;
    float right_margin = 4.0f;
    float top_margin = 4.0f;
    float bottom_margin = 4.0f;
    math::Vector2f size = math::Vector2f(100.0f, 100.0f);
    render::Color modulate = render::Color::white();

    NinePatchRect() = default;
    const char* type() const override { return "NinePatchRect"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["texture_path"] = texture_path;
        out["left_margin"] = left_margin;
        out["right_margin"] = right_margin;
        out["top_margin"] = top_margin;
        out["bottom_margin"] = bottom_margin;
        out["size"] = { size.x, size.y };
        out["modulate"] = { modulate.r, modulate.g, modulate.b, modulate.a };
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        texture_path = in.value("texture_path", "");
        left_margin = in.value("left_margin", 4.0f);
        right_margin = in.value("right_margin", 4.0f);
        top_margin = in.value("top_margin", 4.0f);
        bottom_margin = in.value("bottom_margin", 4.0f);
        auto s = in.value("size", std::vector<float>{100, 100});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        auto c = in.value("modulate", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) modulate = render::Color(c[0], c[1], c[2], c[3]);
    }
};

// ---------------------------------------------------------------------------
// LightOccluder2D — 2D 光遮挡/阴影投射体（配合 Light2D 阴影系统）
// ---------------------------------------------------------------------------
class LightOccluder2D : public Component2D {
public:
    std::vector<math::Vector2f> points;
    bool visible = true;

    LightOccluder2D() = default;
    const char* type() const override { return "LightOccluder2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["visible"] = visible;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : points) arr.push_back({ p.x, p.y });
        out["points"] = std::move(arr);
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        visible = in.value("visible", true);
        points.clear();
        if (in.contains("points") && in["points"].is_array()) {
            for (const auto& item : in["points"]) {
                if (item.is_array() && item.size() >= 2) {
                    points.emplace_back(item[0].get<float>(), item[1].get<float>());
                }
            }
        }
    }
};

} // namespace gryce_engine::components::d2
