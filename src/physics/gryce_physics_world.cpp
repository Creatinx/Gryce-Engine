#include "GryceEngineUtils/physics.h"

#include "physics/physics_world_3d.h"
#include "physics/physics_factory.h"

namespace GryceEngineUtils {

PhysicsWorld::PhysicsWorld() {
    world_ = gryce_engine::physics::create_physics_world_3d("jolt");
    if (world_) {
        world_->init(math::Vector3f(0.0f, -9.81f, 0.0f));
    }
}

PhysicsWorld::~PhysicsWorld() {
    if (world_) {
        world_->shutdown();
    }
}

PhysicsWorld* PhysicsWorld::create() {
    return new PhysicsWorld();
}

void PhysicsWorld::destroy() {
    delete this;
}

void PhysicsWorld::step(float dt) {
    if (world_) {
        world_->step(dt);
    }
}

void PhysicsWorld::set_gravity(const math::Vector3f& gravity) {
    if (world_) {
        world_->set_gravity(gravity);
    }
}

} // namespace GryceEngineUtils
