#pragma once

// GryceEngineUtils::physics.h — 可选物理模块入口
//
// PhysicsWorld 是物理引擎（Jolt 3D / Box2D 2D）的独立包装，
// 不依赖 ECS，可单独在裸框架中使用：
//
//   auto* physics = GryceEngineUtils::PhysicsWorld::create();
//   physics->step(dt);
//   physics->destroy();

#include <memory>

#include "GryceEngineUtils/math.h"

namespace gryce_engine::physics { class IPhysicsWorld3D; }

namespace GryceEngineUtils {

class PhysicsWorld {
public:
    static PhysicsWorld* create();
    void destroy();
    void step(float dt);

    // 后续添加：raycast, add_body, remove_body, set_gravity 等
    void set_gravity(const math::Vector3f& gravity);

private:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    std::unique_ptr<gryce_engine::physics::IPhysicsWorld3D> world_;
};

} // namespace GryceEngineUtils
