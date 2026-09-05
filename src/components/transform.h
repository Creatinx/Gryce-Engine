#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Transform — 变换组件
// 每个 Entity 默认带一个 Transform，表示本地/世界空间的位置、旋转、缩放。
// ---------------------------------------------------------------------------
class Transform : public Component {
public:
    math::Vector3f position;
    math::Quaternionf rotation = math::Quaternionf::identity();
    math::Vector3f scale = math::Vector3f::one();

    Transform() = default;

    const char* type() const override { return "Transform"; }

    void serialize(nlohmann::json& out) const override {
        out["position"] = { position.x, position.y, position.z };
        out["rotation"] = { rotation.x, rotation.y, rotation.z, rotation.w };
        out["scale"] = { scale.x, scale.y, scale.z };
    }

    void deserialize(const nlohmann::json& in) override {
        auto pos = in.value("position", std::vector<float>{0, 0, 0});
        if (pos.size() >= 3) position = math::Vector3f(pos[0], pos[1], pos[2]);

        auto rot = in.value("rotation", std::vector<float>{0, 0, 0, 1});
        if (rot.size() >= 4) rotation = math::Quaternionf(rot[0], rot[1], rot[2], rot[3]);

        auto scl = in.value("scale", std::vector<float>{1, 1, 1});
        if (scl.size() >= 3) scale = math::Vector3f(scl[0], scl[1], scl[2]);
    }

    // 本地变换矩阵
    math::Matrix4f local_matrix() const {
        return math::Matrix4f::translate(position) *
               math::Matrix4f::from_quaternion(rotation) *
               math::Matrix4f::scale(scale);
    }

    // 世界变换矩阵（需要父级世界矩阵）
    math::Matrix4f world_matrix(const math::Matrix4f& parent_world) const {
        return parent_world * local_matrix();
    }
};

} // namespace gryce_engine::components
