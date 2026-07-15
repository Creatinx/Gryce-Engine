#pragma once

#include "components/component.h"
#include "math/math.h"
#include "physics/physics_types.h"
#include "scene/uuid.h"
#include <string>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Joint2D — 2D 关节组件
// 连接两个拥有刚体的实体。通过 body_a_uuid / body_b_uuid 在场景中查找目标实体。
// ---------------------------------------------------------------------------
class Joint2D : public Component {
public:
    // 被连接的两个实体的 UUID
    scene::UUID body_a_uuid;
    scene::UUID body_b_uuid;

    // 关节类型
    physics::JointType joint_type = physics::JointType::Fixed;

    // 两个锚点（相对于各自实体本地坐标系）
    math::Vector2f anchor_a;
    math::Vector2f anchor_b;

    // Distance / Spring 的静止长度
    float length = 1.0f;
    // Spring 频率与阻尼
    float frequency = 2.0f;
    float damping = 0.5f;

    // 连接体之间是否允许碰撞
    bool collide_connected = false;

    Joint2D() = default;

    const char* type() const override { return "Joint2D"; }

    void serialize(nlohmann::json& out) const override {
        out["body_a_uuid"] = body_a_uuid.str();
        out["body_b_uuid"] = body_b_uuid.str();
        out["joint_type"] = static_cast<int>(joint_type);
        out["anchor_a"] = { anchor_a.x, anchor_a.y };
        out["anchor_b"] = { anchor_b.x, anchor_b.y };
        out["length"] = length;
        out["frequency"] = frequency;
        out["damping"] = damping;
        out["collide_connected"] = collide_connected;
    }

    void deserialize(const nlohmann::json& in) override {
        body_a_uuid = scene::UUID(in.value("body_a_uuid", ""));
        body_b_uuid = scene::UUID(in.value("body_b_uuid", ""));
        joint_type = static_cast<physics::JointType>(in.value("joint_type", 0));
        auto a = in.value("anchor_a", std::vector<float>{0, 0});
        if (a.size() >= 2) anchor_a = math::Vector2f(a[0], a[1]);
        auto b = in.value("anchor_b", std::vector<float>{0, 0});
        if (b.size() >= 2) anchor_b = math::Vector2f(b[0], b[1]);
        length = in.value("length", 1.0f);
        frequency = in.value("frequency", 2.0f);
        damping = in.value("damping", 0.5f);
        collide_connected = in.value("collide_connected", false);
    }
};

} // namespace gryce_engine::components
