#pragma once

#include "components/2d/component_2d.h"
#include "math/math.h"

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// Marker2D — 标记点（出生点、挂点、编辑器辅助）
// ---------------------------------------------------------------------------
class Marker2D : public Component2D {
public:
    std::string marker_name;
    bool show_in_editor = true;

    Marker2D() = default;
    const char* type() const override { return "Marker2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["marker_name"] = marker_name;
        out["show_in_editor"] = show_in_editor;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        marker_name = in.value("marker_name", "");
        show_in_editor = in.value("show_in_editor", true);
    }
};

// ---------------------------------------------------------------------------
// VisibilityNotifier2D — 屏幕可见通知（对象池/剔除；渲染系统填充 is_visible）
// ---------------------------------------------------------------------------
class VisibilityNotifier2D : public Component2D {
public:
    math::Vector2f size = math::Vector2f::one();
    bool is_visible = false;

    VisibilityNotifier2D() = default;
    const char* type() const override { return "VisibilityNotifier2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["size"] = { size.x, size.y };
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        auto s = in.value("size", std::vector<float>{1, 1});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
    }
};

} // namespace gryce_engine::components::d2
