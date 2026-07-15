#pragma once

#include "ecs/system.h"
#include "math/math.h"
#include "physics/physics_types.h"
#include <memory>
#include <optional>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// PhysicsSystem3D — 3D 物理系统
// 底层委托给 IPhysicsWorld3D（当前默认 Jolt），支持 RigidBody / StaticBody +
// BoxCollider / SphereCollider / PlaneCollider，含重力、阻尼、睡眠。
// ---------------------------------------------------------------------------
class PhysicsSystem3D : public ISystem {
public:
    PhysicsSystem3D();
    ~PhysicsSystem3D() override;

    const char* name() const override { return "PhysicsSystem3D"; }
    Phase phase() const override { return Phase::Update; }

    void on_init(scene::Scene& scene) override;
    void on_shutdown(scene::Scene& scene) override;
    void on_update(scene::Scene& scene, float dt) override;

    // 射线检测：从 origin 沿 direction 发射射线，最大距离 max_distance（单位：米）
    std::optional<physics::RaycastHit> raycast(const math::Vector3f& origin,
                                                const math::Vector3f& direction,
                                                float max_distance) const;

    // 全局重力（默认 -9.81 m/s^2，沿 Y 轴向下）
    math::Vector3f gravity = math::Vector3f(0.0f, -9.81f, 0.0f);

    // 固定物理步长
    float fixed_dt = 1.0f / 60.0f;
    // 每个固定步内部的子步数
    int substeps = 4;
    // 单帧最大物理步数
    int max_steps_per_frame = 8;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gryce_engine::ecs
