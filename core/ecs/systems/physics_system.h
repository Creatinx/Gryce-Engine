#pragma once

#include "ecs/system.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// PhysicsSystem — 点云物理模拟系统
// 对所有挂载 PhysicsBody 的 Entity 做 Verlet 积分 + 简单地面碰撞。
// ---------------------------------------------------------------------------
class PhysicsSystem : public ISystem {
public:
    const char* name() const override { return "PhysicsSystem"; }
    Phase phase() const override { return Phase::Update; }

    void on_update(scene::Scene& scene, float dt) override;
};

} // namespace gryce_engine::ecs
