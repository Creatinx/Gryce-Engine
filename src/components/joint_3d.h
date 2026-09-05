#pragma once

#include "components/component.h"
#include "math/math.h"
#include "physics/physics_types.h"
#include "scene/uuid.h"
#include <string>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Joint3D — 3D 关节组件
// 连接两个拥有刚体的实体。通过 body_a_uuid / body_b_uuid 在场景中查找目标实体。
// ---------------------------------------------------------------------------
class Joint3D : public Component {
public:
    // 被连接的两个实体的 UUID
    scene::UUID body_a_uuid;
    scene::UUID body_b_uuid;

    // 关节类型
    physics::JointType joint_type = physics::JointType::Fixed;

    // 两个锚点（相对于各自实体本地坐标系）
    math::Vector3f anchor_a;
    math::Vector3f anchor_b;

    // Hinge 转轴
    math::Vector3f axis_a{0.0f, 0.0f, 1.0f};
    math::Vector3f axis_b{0.0f, 0.0f, 1.0f};

    // Distance / Spring 的静止长度
    float length = 1.0f;
    // Spring 频率与阻尼
    float frequency = 2.0f;
    float damping = 0.5f;

    // 连接体之间是否允许碰撞
    bool collide_connected = false;

    Joint3D() = default;

    const char* type() const override { return "Joint3D"; }

    void serialize(nlohmann::json& out) const override {
        out["body_a_uuid"] = body_a_uuid.str();
        out["body_b_uuid"] = body_b_uuid.str();
        out["joint_type"] = static_cast<int>(joint_type);
        out["anchor_a"] = { anchor_a.x, anchor_a.y, anchor_a.z };
        out["anchor_b"] = { anchor_b.x, anchor_b.y, anchor_b.z };
        out["axis_a"] = { axis_a.x, axis_a.y, axis_a.z };
        out["axis_b"] = { axis_b.x, axis_b.y, axis_b.z };
        out["length"] = length;
        out["frequency"] = frequency;
        out["damping"] = damping;
        out["collide_connected"] = collide_connected;
    }

    void deserialize(const nlohmann::json& in) override {
        body_a_uuid = scene::UUID(in.value("body_a_uuid", ""));
        body_b_uuid = scene::UUID(in.value("body_b_uuid", ""));
        joint_type = static_cast<physics::JointType>(in.value("joint_type", 0));
        auto a = in.value("anchor_a", std::vector<float>{0, 0, 0});
        if (a.size() >= 3) anchor_a = math::Vector3f(a[0], a[1], a[2]);
        auto b = in.value("anchor_b", std::vector<float>{0, 0, 0});
        if (b.size() >= 3) anchor_b = math::Vector3f(b[0], b[1], b[2]);
        auto ax_a = in.value("axis_a", std::vector<float>{0, 0, 1});
        if (ax_a.size() >= 3) axis_a = math::Vector3f(ax_a[0], ax_a[1], ax_a[2]);
        auto ax_b = in.value("axis_b", std::vector<float>{0, 0, 1});
        if (ax_b.size() >= 3) axis_b = math::Vector3f(ax_b[0], ax_b[1], ax_b[2]);
        length = in.value("length", 1.0f);
        frequency = in.value("frequency", 2.0f);
        damping = in.value("damping", 0.5f);
        collide_connected = in.value("collide_connected", false);
    }
};

} // namespace gryce_engine::components
