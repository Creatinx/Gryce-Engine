#pragma once

#include "math/math.h"

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// PhysicsPoint — 物理点位
// 从模型顶点导出，供后续物理引擎（质点/粒子/柔体）消费。
// 当前仅做数据预留，不参与模拟。
// ---------------------------------------------------------------------------
struct PhysicsPoint {
    math::Vector3f position;       // 当前位置
    math::Vector3f old_position;   // 上一帧位置（Verlet 积分用）
    math::Vector3f acceleration;   // 外力加速度
    float mass = 1.0f;             // 质量
    bool pinned = false;           // 是否固定

    PhysicsPoint() = default;
    explicit PhysicsPoint(const math::Vector3f& pos, float m = 1.0f, bool p = false)
        : position(pos)
        , old_position(pos)
        , acceleration(math::Vector3f::zero())
        , mass(m)
        , pinned(p) {}
};

} // namespace gryce_engine::physics
