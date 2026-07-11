#include <gtest/gtest.h>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/destructible_body.h"
#include "components/fragment_body.h"
#include "ecs/systems/fracture_system.h"
#include "ecs/systems/physics_system.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/math.h"

using namespace gryce_engine;

TEST(Fracture, FracturesWhenImpulseExceedsThreshold) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    scene::Entity* cube = scene.create_entity("Cube");
    cube->transform()->position = math::Vector3f(0.0f, 0.6f, 0.0f);
    cube->add_component<components::RigidBody>();
    auto* col = cube->add_component<components::BoxCollider>();
    col->size = math::Vector3f(1.0f, 1.0f, 1.0f);
    auto* destructible = cube->add_component<components::DestructibleBody>();
    destructible->fracture_threshold = 1.0f;
    destructible->segments = math::Vector3i(2, 2, 2);
    destructible->max_fragments = 8;
    destructible->fragment_lifetime = -1.0f; // 永久，方便计数

    // 手动设置上一帧碰撞冲量超过阈值
    cube->get_component<components::RigidBody>()->last_collision_impulse = 5.0f;

    ecs::FractureSystem sys;
    sys.on_update(scene, 0.016f);

    // 原 Cube 应被销毁
    EXPECT_EQ(scene.find_entity_by_name("Cube"), nullptr);

    // 应生成 8 个名为 Fragment 的碎片实体
    int fragment_count = 0;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->name().find("Fragment") != std::string::npos) {
            ++fragment_count;
        }
    });
    EXPECT_EQ(fragment_count, 8);
}

TEST(Fracture, DoesNotFractureBelowThreshold) {
    scene::Scene scene("test");

    scene::Entity* cube = scene.create_entity("Cube");
    cube->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    cube->add_component<components::RigidBody>();
    cube->add_component<components::BoxCollider>();
    auto* destructible = cube->add_component<components::DestructibleBody>();
    destructible->fracture_threshold = 10.0f;

    cube->get_component<components::RigidBody>()->last_collision_impulse = 1.0f;

    ecs::FractureSystem sys;
    sys.on_update(scene, 0.016f);

    EXPECT_NE(scene.find_entity_by_name("Cube"), nullptr);
}

TEST(Fracture, FragmentsExpireAfterLifetime) {
    scene::Scene scene("test");

    scene::Entity* cube = scene.create_entity("Cube");
    cube->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    cube->add_component<components::RigidBody>();
    cube->add_component<components::BoxCollider>();
    auto* destructible = cube->add_component<components::DestructibleBody>();
    destructible->fracture_threshold = 1.0f;
    destructible->segments = math::Vector3i(2, 1, 1);
    destructible->max_fragments = 2;
    destructible->fragment_lifetime = 0.5f;

    cube->get_component<components::RigidBody>()->last_collision_impulse = 5.0f;

    ecs::FractureSystem sys;
    sys.on_update(scene, 0.016f);

    int fragments_alive = 0;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->get_component<components::FragmentBody>()) {
            ++fragments_alive;
        }
    });
    EXPECT_EQ(fragments_alive, 2);

    // 超过生命周期
    sys.on_update(scene, 1.0f);

    fragments_alive = 0;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->get_component<components::FragmentBody>()) {
            ++fragments_alive;
        }
    });
    EXPECT_EQ(fragments_alive, 0);
}

TEST(Fracture, PhysicsCollisionTriggersFracture) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    scene::Entity* cube = scene.create_entity("Cube");
    cube->transform()->position = math::Vector3f(0.0f, 5.0f, 0.0f);
    auto* rb = cube->add_component<components::RigidBody>();
    rb->restitution = 0.1f;
    auto* col = cube->add_component<components::BoxCollider>();
    col->size = math::Vector3f(1.0f, 1.0f, 1.0f);
    auto* destructible = cube->add_component<components::DestructibleBody>();
    destructible->fracture_threshold = 3.0f;
    destructible->segments = math::Vector3i(2, 2, 2);
    destructible->max_fragments = 8;
    destructible->fragment_lifetime = -1.0f;

    ecs::PhysicsSystem physics;
    ecs::FractureSystem fracture;

    // 模拟下落 + 撞击 + 碎裂
    for (int i = 0; i < 120; ++i) {
        physics.on_update(scene, 0.016f);
        fracture.on_update(scene, 0.016f);
        if (scene.find_entity_by_name("Cube") == nullptr) break;
    }

    EXPECT_EQ(scene.find_entity_by_name("Cube"), nullptr);

    int fragment_count = 0;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->name().find("Fragment") != std::string::npos) {
            ++fragment_count;
        }
    });
    EXPECT_GT(fragment_count, 0);
}
