#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// CharacterController3D — 3D FPS/第三人称角色控制器
// 需配合 RigidBody 与 3D 碰撞体使用。系统每帧读取 move_input / jump_requested，
// 通过设置刚体水平速度并施加上跳冲量来驱动角色。
// ---------------------------------------------------------------------------
class CharacterController3D : public Component {
public:
    // 移动速度（米/秒）
    float speed = 5.0f;
    // 起跳冲量（米/秒）
    float jump_force = 8.0f;
    // 接地检测射线起点偏移（相对于角色中心）
    math::Vector3f ground_check_offset = math::Vector3f::zero();
    // 接地检测射线长度
    float ground_check_distance = 0.15f;
    // 接地检测水平采样半径（在角色中心前后左右 cast 多根射线）
    float ground_check_radius = 0.25f;
    // 是否固定旋转（通常保持 true）
    bool fixed_rotation = true;

    // 坡度限制（度）
    float slope_limit_degrees = 60.0f;
    // 台阶高度（米）
    float step_height = 0.25f;
    // 外部推撞速度的保留/恢复速度
    float push_recovery_speed = 8.0f;

    // 运行时输入（不序列化，由脚本/输入系统每帧写入）
    math::Vector3f move_input = math::Vector3f::zero();
    bool jump_requested = false;

    // 运行时状态（只读）
    bool is_grounded = false;
    math::Vector3f ground_normal = math::Vector3f(0.0f, 1.0f, 0.0f);

    CharacterController3D() = default;

    const char* type() const override { return "CharacterController3D"; }

    void serialize(nlohmann::json& out) const override {
        out["speed"] = speed;
        out["jump_force"] = jump_force;
        out["ground_check_offset"] = { ground_check_offset.x, ground_check_offset.y, ground_check_offset.z };
        out["ground_check_distance"] = ground_check_distance;
        out["ground_check_radius"] = ground_check_radius;
        out["fixed_rotation"] = fixed_rotation;
        out["slope_limit_degrees"] = slope_limit_degrees;
        out["step_height"] = step_height;
        out["push_recovery_speed"] = push_recovery_speed;
    }

    void deserialize(const nlohmann::json& in) override {
        speed = in.value("speed", 5.0f);
        jump_force = in.value("jump_force", 8.0f);
        auto off = in.value("ground_check_offset", std::vector<float>{0, 0, 0});
        if (off.size() >= 3) ground_check_offset = math::Vector3f(off[0], off[1], off[2]);
        ground_check_distance = in.value("ground_check_distance", 0.15f);
        ground_check_radius = in.value("ground_check_radius", 0.25f);
        fixed_rotation = in.value("fixed_rotation", true);
        slope_limit_degrees = in.value("slope_limit_degrees", 60.0f);
        step_height = in.value("step_height", 0.25f);
        push_recovery_speed = in.value("push_recovery_speed", 8.0f);
    }
};

} // namespace gryce_engine::components
