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
    // 相对于 owner position 的偏移
    math::Vector2f offset = math::Vector2f::zero();

    Camera2D() = default;

    const char* type() const override { return "Camera2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["is_active"] = is_active;
        out["zoom"] = zoom;
        out["offset"] = { offset.x, offset.y };
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        is_active = in.value("is_active", true);
        zoom = in.value("zoom", 1.0f);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
    }

    // 获取摄像机中心世界坐标
    math::Vector2f center() const {
        math::Vector2f p = position();
        return p + offset;
    }

    void draw(render::IRenderer2D* /*renderer*/) override {
        // 摄像机本身不绘制
    }

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, static_cast<uint64_t>(is_active));
        hash_combine(h, hash_float(zoom));
        hash_combine(h, hash_float(offset.x));
        hash_combine(h, hash_float(offset.y));
        return h;
    }
};

} // namespace gryce_engine::components::d2::camera
