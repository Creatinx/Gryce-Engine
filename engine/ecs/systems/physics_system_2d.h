#pragma once

#include "export.h"
#include "ecs/system.h"
#include "math/math.h"

#include <memory>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// PhysicsSystem2D — 2D 物理系统（Box2D v3）
//
// 每帧流程：
//   1. 为拥有 RigidBody2D 的实体创建/销毁底层 b2Body（含碰撞体）
//   2. 以固定步长推进世界；每步先施加风力与浮力，再 b2World_Step
//   3. 把刚体位姿回写到 Transform，速度回写到 RigidBody2D
//
// 风力：WindZone2D 对区域内动态刚体施加"加速度"（引擎单位/秒²），
//       另可叠加湍流与速度阻尼。
// 浮力：BuoyancyArea2D 按阿基米德原理对浸没部分施加浮力 + 流体阻力，
//       浸没比例由水面高度与刚体包围盒求得。
//
// 单位：Transform/组件使用引擎单位（默认像素），系统按 pixels_per_meter
// 换算到 Box2D 的米制世界；重力、风力均以引擎单位表达，与像素场景直观一致。
// ---------------------------------------------------------------------------
class GRYCE_API PhysicsSystem2D : public ISystem {
public:
    PhysicsSystem2D();
    ~PhysicsSystem2D() override;

    const char* name() const override { return "PhysicsSystem2D"; }
    Phase phase() const override { return Phase::Update; }
    // 负优先级：物理在业务逻辑（默认 0）之后、动画等之前推进
    int priority() const override { return -50; }

    void on_init(scene::Scene& scene) override;
    void on_shutdown(scene::Scene& scene) override;
    void on_update(scene::Scene& scene, float dt) override;

    // 重力（引擎单位/秒²）。默认 +Y 向下，匹配 2D 屏幕坐标系。
    void set_gravity(const math::Vector2f& g) override;
    math::Vector2f get_gravity() const override;

    // 引擎单位 -> 米。Box2D 建议刚体尺寸落在 0.1~10 米，像素场景取 100。
    float pixels_per_meter = 100.0f;
    // 固定物理步长与每步子步数（子步越多越稳定，代价越高）
    float fixed_timestep = 1.0f / 60.0f;
    int sub_step_count = 4;
    // 单帧最多追赶的物理步数，避免卡顿后的"死亡螺旋"
    int max_steps_per_frame = 5;

private:
    // 对区域内动态刚体施加风力 / 浮力（ppm = 引擎单位每米）
    void apply_wind(scene::Scene& scene, float dt, float ppm);
    void apply_buoyancy(scene::Scene& scene, float dt, float ppm);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gryce_engine::ecs
