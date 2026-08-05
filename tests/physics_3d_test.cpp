#include <gtest/gtest.h>

#include <algorithm>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/character_controller_3d.h"
#include "components/destructible_body.h"
#include "components/joint_3d.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/fracture_system.h"
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

TEST(Physics3D, RaycastHitsStaticBox) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(4.0f, 1.0f, 4.0f);

    ecs::PhysicsSystem3D sys;
    sys.on_init(scene);
    sys.on_update(scene, 0.01f);

    auto hit = sys.raycast(math::Vector3f(0.0f, 2.0f, 0.0f), math::Vector3f(0.0f, -1.0f, 0.0f), 5.0f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NE(hit->body, physics::k_invalid_body);
    EXPECT_NEAR(hit->point.y, 0.5f, 1e-3f);
    EXPECT_NEAR(hit->normal.y, 1.0f, 1e-3f);
}

TEST(Physics3D, CharacterControllerMovesHorizontally) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    player->add_component<components::RigidBody>();
    player->add_component<components::BoxCollider>();
    auto* cc = player->add_component<components::CharacterController3D>();
    cc->speed = 4.0f;
    cc->move_input = math::Vector3f(1.0f, 0.0f, 0.0f);

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 120;
    sys.on_init(scene);
    sys.on_update(scene, 0.5f);

    EXPECT_GT(player->transform()->position.x, 0.5f);
    EXPECT_LT(player->transform()->position.y, 2.0f);
}

TEST(Physics3D, DistanceJointConstrainsBodies) {
    scene::Scene scene("test");

    scene::Entity* anchor = scene.create_entity("Anchor");
    anchor->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    anchor->add_component<components::StaticBody>();
    anchor->add_component<components::BoxCollider>();

    scene::Entity* bob = scene.create_entity("Bob");
    bob->transform()->position = math::Vector3f(2.0f, 0.0f, 0.0f);
    bob->add_component<components::RigidBody>();
    bob->add_component<components::BoxCollider>();

    scene::Entity* joint_entity = scene.create_entity("Joint");
    auto* joint = joint_entity->add_component<components::Joint3D>();
    joint->body_a_uuid = anchor->uuid();
    joint->body_b_uuid = bob->uuid();
    joint->joint_type = physics::JointType::Distance;
    joint->length = 1.5f;
    joint->anchor_a = math::Vector3f::zero();
    joint->anchor_b = math::Vector3f::zero();

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240;
    sys.on_init(scene);
    sys.on_update(scene, 2.0f);

    math::Vector3f diff = bob->transform()->position - anchor->transform()->position;
    float dist = diff.length();
    EXPECT_NEAR(dist, 1.5f, 0.05f);
}

TEST(Physics3D, CharacterControllerJumpsWhenGrounded) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody>();
    auto* box = player->add_component<components::BoxCollider>();
    box->size = math::Vector3f(1.0f, 1.0f, 1.0f);
    auto* cc = player->add_component<components::CharacterController3D>();
    cc->ground_check_distance = 0.55f;
    cc->jump_force = 6.0f;
    cc->jump_requested = true;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 120;
    sys.on_init(scene);
    sys.on_update(scene, 0.1f);

    EXPECT_TRUE(cc->is_grounded);
    auto* rb = player->get_component<components::RigidBody>();
    EXPECT_GT(rb->velocity.y, 1.0f);
}

TEST(Physics3D, RaycastMissReturnsNullopt) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(4.0f, 1.0f, 4.0f);

    ecs::PhysicsSystem3D sys;
    sys.on_init(scene);
    sys.on_update(scene, 0.01f);

    auto hit = sys.raycast(math::Vector3f(10.0f, 2.0f, 0.0f), math::Vector3f(0.0f, -1.0f, 0.0f), 5.0f);
    EXPECT_FALSE(hit.has_value());
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

TEST(Physics3D, CharacterControllerPreservesPushVelocity) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody>();
    player->add_component<components::BoxCollider>();
    auto* cc = player->add_component<components::CharacterController3D>();
    cc->ground_check_distance = 0.55f;
    cc->push_recovery_speed = 2.0f;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 120;
    sys.on_init(scene);
    sys.on_update(scene, 0.05f);

    auto* rb = player->get_component<components::RigidBody>();
    rb->velocity = math::Vector3f(3.0f, 0.0f, 0.0f);
    sys.on_update(scene, 0.1f);

    EXPECT_GT(rb->velocity.x, 0.5f);
}

TEST(Physics3D, CharacterControllerStepsUpStair) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody>();
    auto* ground_col = ground->add_component<components::BoxCollider>();
    ground_col->size = math::Vector3f(10.0f, 1.0f, 4.0f);

    scene::Entity* step = scene.create_entity("Step");
    step->transform()->position = math::Vector3f(1.5f, 0.4f, 0.0f);
    step->add_component<components::StaticBody>();
    auto* step_col = step->add_component<components::BoxCollider>();
    step_col->size = math::Vector3f(0.6f, 0.3f, 1.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody>();
    player->add_component<components::BoxCollider>();
    auto* cc = player->add_component<components::CharacterController3D>();
    cc->speed = 3.0f;
    cc->ground_check_distance = 0.55f;
    cc->step_height = 0.35f;
    cc->move_input = math::Vector3f(1.0f, 0.0f, 0.0f);

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240;
    sys.on_init(scene);
    sys.on_update(scene, 1.0f);

    EXPECT_GT(player->transform()->position.x, 1.0f);
}

TEST(Physics3D, JointDestroyedWhenBodyRemoved) {
    scene::Scene scene("test");

    scene::Entity* anchor = scene.create_entity("Anchor");
    anchor->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    anchor->add_component<components::StaticBody>();
    anchor->add_component<components::BoxCollider>();

    scene::Entity* bob = scene.create_entity("Bob");
    bob->transform()->position = math::Vector3f(2.0f, 0.0f, 0.0f);
    bob->add_component<components::RigidBody>();
    bob->add_component<components::BoxCollider>();

    scene::Entity* joint_entity = scene.create_entity("Joint");
    auto* joint = joint_entity->add_component<components::Joint3D>();
    joint->body_a_uuid = anchor->uuid();
    joint->body_b_uuid = bob->uuid();
    joint->joint_type = physics::JointType::Distance;
    joint->length = 1.5f;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 120;
    sys.on_init(scene);
    sys.on_update(scene, 0.1f);

    scene.destroy_entity(bob);
    sys.on_update(scene, 0.1f);

    SUCCEED();
}

TEST(Physics3D, TriggerFiresEnterAndExitEvents) {
    scene::Scene scene("test");

    // 静态触发器盒子（is_trigger），动态小球穿过它
    scene::Entity* zone = scene.create_entity("TriggerZone");
    zone->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    zone->add_component<components::StaticBody>();
    auto* zone_col = zone->add_component<components::BoxCollider>();
    zone_col->size = math::Vector3f(3.0f, 2.0f, 3.0f);
    zone_col->is_trigger = true;

    scene::Entity* ball = scene.create_entity("Ball");
    ball->transform()->position = math::Vector3f(0.0f, 10.0f, 0.0f);
    ball->add_component<components::RigidBody>();
    ball->add_component<components::SphereCollider>();

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240;
    sys.on_init(scene);

    bool entered = false;
    bool exited = false;
    bool trigger_flag_ok = false;
    for (int frame = 0; frame < 240; ++frame) {
        sys.on_update(scene, 1.0f / 60.0f);
        for (const auto& ev : sys.collision_events()) {
            if (ev.is_trigger) {
                trigger_flag_ok = true;
                if (ev.type == physics::CollisionEventType::BeginContact) entered = true;
                if (ev.type == physics::CollisionEventType::EndContact) exited = true;
            }
        }
    }

    // 小球应自由穿过触发器（无碰撞响应），因此最终会落到原点以下
    EXPECT_LT(ball->transform()->position.y, -1.0f);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(exited);
    EXPECT_TRUE(trigger_flag_ok);
}

TEST(Physics3D, CollisionEventsReportImpulse) {
    scene::Scene scene("test");

    scene::Entity* a = scene.create_entity("A");
    a->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    auto* rb_a = a->add_component<components::RigidBody>();
    rb_a->velocity = math::Vector3f(5.0f, 0.0f, 0.0f);
    rb_a->restitution = 0.5f;
    a->add_component<components::SphereCollider>();

    scene::Entity* b = scene.create_entity("B");
    b->transform()->position = math::Vector3f(1.5f, 0.0f, 0.0f);
    auto* rb_b = b->add_component<components::RigidBody>();
    rb_b->velocity = math::Vector3f(-5.0f, 0.0f, 0.0f);
    rb_b->restitution = 0.5f;
    b->add_component<components::SphereCollider>();

    ecs::PhysicsSystem3D sys;
    sys.on_init(scene);

    bool saw_begin = false;
    bool saw_end = false;
    float max_impulse = 0.0f;
    for (int frame = 0; frame < 30; ++frame) {
        sys.on_update(scene, 1.0f / 60.0f);
        for (const auto& ev : sys.collision_events()) {
            if (ev.is_trigger) continue;
            if (ev.type == physics::CollisionEventType::BeginContact) {
                saw_begin = true;
                max_impulse = std::max(max_impulse, ev.impulse);
            } else {
                saw_end = true;
            }
        }
        max_impulse = std::max(max_impulse, rb_a->last_collision_impulse);
    }

    EXPECT_TRUE(saw_begin);
    EXPECT_TRUE(saw_end);
    // 两个 1kg 球相向 5m/s 碰撞，冲量应在几 kg·m/s 量级
    EXPECT_GT(max_impulse, 0.5f);
}

TEST(Physics3D, DestructibleBodyFracturesOnHardImpact) {
    scene::Scene scene("test");

    // 地面
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody>();
    auto* gc = ground->add_component<components::BoxCollider>();
    gc->size = math::Vector3f(10.0f, 1.0f, 10.0f);

    // 可碎裂盒子从高处砸向地面
    scene::Entity* cube = scene.create_entity("Cube");
    cube->transform()->position = math::Vector3f(0.0f, 8.0f, 0.0f);
    auto* rb = cube->add_component<components::RigidBody>();
    rb->restitution = 0.0f;
    cube->add_component<components::BoxCollider>();
    auto* destructible = cube->add_component<components::DestructibleBody>();
    destructible->fracture_threshold = 0.5f;

    ecs::PhysicsSystem3D sys;
    ecs::FractureSystem fracture;
    sys.max_steps_per_frame = 240;
    sys.on_init(scene);

    bool fractured = false;
    for (int frame = 0; frame < 240 && !fractured; ++frame) {
        sys.on_update(scene, 1.0f / 60.0f);
        // 物理系统先写入 last_collision_impulse，FractureSystem 随后判定碎裂
        fractured = destructible->fractured;
        if (fractured) break;
        fracture.on_update(scene, 1.0f / 60.0f);
    }

    // 高速落地应产生足够冲量触发碎裂（验证 last_collision_impulse 已接线）
    EXPECT_TRUE(fractured);
}

