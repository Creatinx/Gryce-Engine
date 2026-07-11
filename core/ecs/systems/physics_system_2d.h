#pragma once

#include "ecs/system.h"
#include "math/math.h"
#include <memory>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// PhysicsSystem2D — 2D 物理系统
// 底层委托给 IPhysicsWorld2D（当前默认 Box2D），支持 RigidBody2D +
// BoxCollider2D / CircleCollider2D，含重力、阻尼、睡眠。
// ---------------------------------------------------------------------------
class PhysicsSystem2D : public ISystem {
public:
    PhysicsSystem2D();
    ~PhysicsSystem2D() override;

    const char* name() const override { return "PhysicsSystem2D"; }
    Phase phase() const override { return Phase::Update; }

    void on_update(scene::Scene& scene, float dt) override;

    // 全局重力（默认 -9.81 m/s^2，沿 Y 轴向下）
    math::Vector2f gravity = math::Vector2f(0.0f, -9.81f);

    // 固定物理步长（外部后端如 Box2D 要求使用固定小步长）
    float fixed_dt = 1.0f / 60.0f;
    // 每个固定步内部的子步数
    int substeps = 4;
    // 单帧最大物理步数（防止低帧率时物理爆炸；测试需要可临时调大）
    int max_steps_per_frame = 8;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gryce_engine::ecs
