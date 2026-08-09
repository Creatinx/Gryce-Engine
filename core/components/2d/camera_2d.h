#pragma once

#include "components/2d/component_2d.h"
#include "math/math.h"

namespace gryce_engine::components::d2::camera {

// ---------------------------------------------------------------------------
// Camera2D — 2D 摄像机组件。
// - 挂载到 Entity 上，以该 Entity 的 Transform.position 为中心（也可覆盖）
// - zoom: 1.0 = 原始尺寸，>1 放大，<1 缩小
// - 若 is_active 为 true，RenderSystem2D 会优先使用它作为当前视图
// ---------------------------------------------------------------------------
class Camera2D : public Component2D {
public:
    bool is_active = true;
    float zoom = 1.0f;
    // 摄像机 Z 旋转（弧度，Godot 语义：正值为顺时针旋转视图）
    float rotation = 0.0f;
    // 相对于 owner position 的偏移
    math::Vector2f offset = math::Vector2f::zero();
    // 摄像机中心限制（Godot Camera2D.limit_*）；enable 后中心被夹在矩形内
    bool limit_enabled = false;
    float limit_left = -1000000.0f;
    float limit_top = -1000000.0f;
    float limit_right = 1000000.0f;
    float limit_bottom = 1000000.0f;

    Camera2D() = default;

    const char* type() const override { return "Camera2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["is_active"] = is_active;
        out["zoom"] = zoom;
        out["rotation"] = rotation;
        out["offset"] = { offset.x, offset.y };
        out["limit_enabled"] = limit_enabled;
        out["limit_left"] = limit_left;
        out["limit_top"] = limit_top;
        out["limit_right"] = limit_right;
        out["limit_bottom"] = limit_bottom;
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        is_active = in.value("is_active", true);
        zoom = in.value("zoom", 1.0f);
        rotation = in.value("rotation", 0.0f);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        limit_enabled = in.value("limit_enabled", false);
        limit_left = in.value("limit_left", -1000000.0f);
        limit_top = in.value("limit_top", -1000000.0f);
        limit_right = in.value("limit_right", 1000000.0f);
        limit_bottom = in.value("limit_bottom", 1000000.0f);
    }

    // 获取摄像机中心世界坐标
    math::Vector2f center() const {
        math::Vector2f p = position();
        p = p + offset;
        if (limit_enabled) {
            p.x = math::clamp(p.x, limit_left, limit_right);
            p.y = math::clamp(p.y, limit_top, limit_bottom);
        }
        return p;
    }

    void draw(render::IRenderer2D* /*renderer*/) override {
        // 摄像机本身不绘制
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, static_cast<uint64_t>(is_active));
        hash_combine(h, hash_float(zoom));
        hash_combine(h, hash_float(rotation));
        hash_combine(h, hash_float(offset.x));
        hash_combine(h, hash_float(offset.y));
        hash_combine(h, static_cast<uint64_t>(limit_enabled));
        hash_combine(h, hash_float(limit_left));
        hash_combine(h, hash_float(limit_top));
        hash_combine(h, hash_float(limit_right));
        hash_combine(h, hash_float(limit_bottom));
        return h;
    }
};

} // namespace gryce_engine::components::d2::camera
