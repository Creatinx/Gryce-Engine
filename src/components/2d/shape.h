#pragma once

#include <vector>

#include "components/2d/component_2d.h"

namespace gryce_engine::components::d2::shape {

// ---------------------------------------------------------------------------
// Circle — 圆形组件
// ---------------------------------------------------------------------------
class Circle : public Component2D {
public:
    float radius = 0.0f;
    int segments = 32;
    render::Color color;

    Circle() = default;
    Circle(float r, int seg, const render::Color& c)
        : radius(r), segments(seg), color(c) {}

    const char* type() const override { return "Circle"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["radius"] = radius;
        out["segments"] = segments;
        out["color"] = { color.r, color.g, color.b, color.a };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        radius = in.value("radius", 0.0f);
        segments = in.value("segments", 32);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    }

    void draw(render::IRenderer2D* renderer) override {
        if (!enabled || !renderer || radius <= 0.0f) return;
        math::Vector2f pos = position();
        renderer->draw_circle(pos.x, pos.y, radius * scale().x, segments, color);
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_float(radius));
        hash_combine(h, static_cast<uint64_t>(segments));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        return h;
    }
};

// ---------------------------------------------------------------------------
// Polygon — 多边形组件
// points 为相对于 position 的局部坐标。
// ---------------------------------------------------------------------------
class Polygon : public Component2D {
public:
    std::vector<math::Vector2f> points;
    render::Color color;

    Polygon() = default;
    Polygon(const std::vector<math::Vector2f>& pts, const render::Color& c)
        : points(pts), color(c) {}

    const char* type() const override { return "Polygon"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : points) {
            pts.push_back({p.x, p.y});
        }
        out["points"] = pts;
        out["color"] = { color.r, color.g, color.b, color.a };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        points.clear();
        if (in.contains("points") && in["points"].is_array()) {
            for (const auto& p : in["points"]) {
                if (p.is_array() && p.size() >= 2) {
                    points.emplace_back(p[0].get<float>(), p[1].get<float>());
                }
            }
        }
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    }

    void draw(render::IRenderer2D* renderer) override {
        if (!enabled || !renderer || points.size() < 3) return;
        std::vector<math::Vector2f> world_points;
        world_points.reserve(points.size());
        for (const auto& p : points) {
            world_points.push_back(transform_point(p));
        }
        renderer->draw_polygon(world_points, color);
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, static_cast<uint64_t>(points.size()));
        for (size_t i = 0; i < std::min<size_t>(points.size(), 8); ++i) {
            hash_combine(h, hash_float(points[i].x));
            hash_combine(h, hash_float(points[i].y));
        }
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        return h;
    }
};

} // namespace gryce_engine::components::d2::shape
