#pragma once

#include "export.h"
#include "ecs/system.h"
#include "math/math.h"

#include <memory>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// PhysicsSystem3D — 3D 物理系统（Jolt Physics）
//
// 每帧流程：
//   1. 为拥有 RigidBody3D 的实体创建/销毁底层 JPH::Body（含碰撞体）
//   2. 以固定步长推进 Jolt 世界（内部分子步保证稳定）
//   3. 把刚体位姿回写到 Transform，速度回写到 RigidBody3D
//
// 单位：引擎世界与 Jolt 同为米制右手系（+Y 向上），Transform 位姿直接下发，
// 无需像素/米换算。质量由碰撞体体积 × Jolt 默认密度自动求得。
//
// 分层：静态物体走 NON_MOVING 层，动态/运动学走 MOVING 层；NON_MOVING 之间
// 不互相检测，节省宽相开销。
// ---------------------------------------------------------------------------
class GRYCE_API PhysicsSystem3D : public ISystem {
public:
    PhysicsSystem3D();
    ~PhysicsSystem3D() override;

    const char* name() const override { return "PhysicsSystem3D"; }
    Phase phase() const override { return Phase::Update; }
    // 负优先级：物理在业务逻辑（默认 0）之后推进
    int priority() const override { return -50; }

    void on_init(scene::Scene& scene) override;
    void on_shutdown(scene::Scene& scene) override;
    void on_update(scene::Scene& scene, float dt) override;

    // 重力（引擎单位/秒²）。默认 -Y 向下（右手系，+Y 向上）。
    // 同时保留基类的 Vector2f 版本（取 x/y，z 置 0）以兼容通用接口。
    using ISystem::set_gravity;
    void set_gravity(const math::Vector3f& g);
    math::Vector3f get_gravity3() const;

    // 世界容量上限（Init 时分配，超出会创建失败）
    int max_bodies = 4096;
    int max_body_pairs = 8192;
    int max_contact_constraints = 2048;
    // 每帧最大物理子步（防止卡顿后追赶过多）
    int max_steps_per_frame = 4;
    // Jolt 工作线程数（0 = 单线程）
    int worker_threads = 2;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gryce_engine::ecs
