#include <gtest/gtest.h>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "ecs/systems/physics_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/math.h"

using namespace gryce_engine;

TEST(Physics3D, RigidBodyFallsUnderGravity) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("Ball");
    e->transform()->position = math::Vector3f(0.0f, 10.0f, 0.0f);
    e->add_component<components::RigidBody>();
    e->add_component<components::SphereCollider>();

    ecs::PhysicsSystem3D sys;
    sys.on_update(scene, 1.0f);

    auto* rb = e->get_component<components::RigidBody>();
    EXPECT_LT(rb->velocity.y, 0.0f);
    EXPECT_LT(e->transform()->position.y, 10.0f);
}

TEST(Physics3D, StaticBodyDoesNotMove) {
    scene::Scene scene("test");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    ground->add_component<components::BoxCollider>();

    ecs::PhysicsSystem3D sys;
    sys.on_update(scene, 1.0f);

    EXPECT_EQ(ground->transform()->position.y, 0.0f);
}

TEST(Physics3D, SphereRestsOnBox) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(2.0f, 1.0f, 2.0f);

    scene::Entity* ball = scene.create_entity("Ball");
    ball->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    auto* rb = ball->add_component<components::RigidBody>();
    rb->mass = 1.0f;
    rb->restitution = 0.1f;
    ball->add_component<components::SphereCollider>();

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240; // 允许 2 秒物理模拟完整跑完
    sys.on_update(scene, 2.0f);

    EXPECT_GT(ball->transform()->position.y, 0.9f);
    EXPECT_LT(ball->transform()->position.y, 1.2f);
    EXPECT_LT(ball->get_component<components::RigidBody>()->velocity.length(), 0.5f);
}

TEST(Physics3D, TwoSpheresCollideAndBounce) {
    scene::Scene scene("test");

    scene::Entity* a = scene.create_entity("A");
    a->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    auto* rb_a = a->add_component<components::RigidBody>();
    rb_a->velocity = math::Vector3f(5.0f, 0.0f, 0.0f);
    rb_a->restitution = 1.0f;
    a->add_component<components::SphereCollider>();

    scene::Entity* b = scene.create_entity("B");
    b->transform()->position = math::Vector3f(1.5f, 0.0f, 0.0f);
    auto* rb_b = b->add_component<components::RigidBody>();
    rb_b->velocity = math::Vector3f(-5.0f, 0.0f, 0.0f);
    rb_b->restitution = 1.0f;
    b->add_component<components::SphereCollider>();

    ecs::PhysicsSystem3D sys;
    sys.on_update(scene, 0.2f);

    EXPECT_LT(rb_a->velocity.x, 0.0f);
    EXPECT_GT(rb_b->velocity.x, 0.0f);
}

TEST(Physics3D, SphereRestsOnPlane) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* plane = ground->add_component<components::PlaneCollider>();
    plane->normal = math::Vector3f::up();
    plane->offset = 0.0f;

    scene::Entity* ball = scene.create_entity("Ball");
    ball->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    auto* rb = ball->add_component<components::RigidBody>();
    rb->mass = 1.0f;
    rb->restitution = 0.0f;
    auto* sphere = ball->add_component<components::SphereCollider>();
    sphere->radius = 0.5f;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240; // 允许 2 秒物理模拟完整跑完
    sys.on_update(scene, 2.0f);

    EXPECT_NEAR(ball->transform()->position.y, -0.5f, 0.15f);
    EXPECT_LT(rb->velocity.length(), 0.1f);
}
