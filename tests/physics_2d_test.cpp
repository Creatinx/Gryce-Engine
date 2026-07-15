#include <gtest/gtest.h>

#ifdef GRYCE_HAS_BOX2D
#include <box2d/box2d.h>
#endif

#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/character_controller_2d.h"
#include "components/joint_2d.h"
#include "components/2d/tilemap.h"
#include "ecs/systems/physics_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/math.h"

using namespace gryce_engine;

#ifdef GRYCE_HAS_BOX2D
// 直接验证 Box2D v3 行为：单个大步长无法得到正确碰撞响应，
// 必须使用固定小步长。这个测试作为引擎使用方式的回归检查。
TEST(Physics2D, RawBox2DCircleRestsOnBox) {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = b2Vec2{0.0f, -9.81f};
    b2WorldId world = b2CreateWorld(&def);

    b2BodyDef ground_def = b2DefaultBodyDef();
    ground_def.position = b2Vec2{0.0f, 0.0f};
    b2BodyId ground = b2CreateBody(world, &ground_def);
    b2Polygon ground_poly = b2MakeOffsetBox(2.0f, 0.5f, b2Vec2{0.0f, 0.0f}, 0.0f);
    b2ShapeDef ground_shape = b2DefaultShapeDef();
    b2CreatePolygonShape(ground, &ground_shape, &ground_poly);

    b2BodyDef ball_def = b2DefaultBodyDef();
    ball_def.type = b2_dynamicBody;
    ball_def.position = b2Vec2{0.0f, 2.0f};
    b2BodyId ball = b2CreateBody(world, &ball_def);
    b2Circle ball_circle;
    ball_circle.center = b2Vec2{0.0f, 0.0f};
    ball_circle.radius = 0.5f;
    b2ShapeDef ball_shape = b2DefaultShapeDef();
    ball_shape.restitution = 0.1f;
    b2CreateCircleShape(ball, &ball_shape, &ball_circle);

    for (int i = 0; i < 120; ++i) {
        b2World_Step(world, 1.0f / 60.0f, 4);
    }
    b2Vec2 p = b2Body_GetPosition(ball);
    b2Vec2 v = b2Body_GetLinearVelocity(ball);

    EXPECT_GT(p.y, 0.9f);
    EXPECT_LT(p.y, 1.2f);
    EXPECT_LT(b2Length(v), 0.5f);

    b2DestroyWorld(world);
}
#endif

TEST(Physics2D, RigidBodyFallsUnderGravity) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("Ball");
    e->transform()->position = math::Vector3f(0.0f, 10.0f, 0.0f);
    e->add_component<components::RigidBody2D>();
    e->add_component<components::CircleCollider2D>();

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 1.0f);

    auto* rb = e->get_component<components::RigidBody2D>();
    EXPECT_LT(rb->velocity.y, 0.0f);
    EXPECT_LT(e->transform()->position.y, 10.0f);
}

TEST(Physics2D, StaticBodyDoesNotMove) {
    scene::Scene scene("test");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    ground->add_component<components::BoxCollider2D>();

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 1.0f);

    EXPECT_EQ(ground->transform()->position.y, 0.0f);
}

TEST(Physics2D, CircleRestsOnBox) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(4.0f, 1.0f);

    scene::Entity* ball = scene.create_entity("Ball");
    ball->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    auto* rb = ball->add_component<components::RigidBody2D>();
    rb->mass = 1.0f;
    rb->restitution = 0.1f;
    auto* circle = ball->add_component<components::CircleCollider2D>();
    circle->radius = 0.5f;

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    EXPECT_GT(ball->transform()->position.y, 0.9f);
    EXPECT_LT(ball->transform()->position.y, 1.2f);
    EXPECT_LT(ball->get_component<components::RigidBody2D>()->velocity.length(), 0.5f);
}

TEST(Physics2D, TwoCirclesCollideAndBounce) {
    scene::Scene scene("test");

    scene::Entity* a = scene.create_entity("A");
    a->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    auto* rb_a = a->add_component<components::RigidBody2D>();
    rb_a->velocity = math::Vector2f(5.0f, 0.0f);
    rb_a->restitution = 1.0f;
    a->add_component<components::CircleCollider2D>();

    scene::Entity* b = scene.create_entity("B");
    b->transform()->position = math::Vector3f(1.5f, 0.0f, 0.0f);
    auto* rb_b = b->add_component<components::RigidBody2D>();
    rb_b->velocity = math::Vector2f(-5.0f, 0.0f);
    rb_b->restitution = 1.0f;
    b->add_component<components::CircleCollider2D>();

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 0.2f);

    EXPECT_LT(rb_a->velocity.x, 0.0f);
    EXPECT_GT(rb_b->velocity.x, 0.0f);
}

TEST(Physics2D, BoxRestsOnBox) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(4.0f, 1.0f);

    scene::Entity* box = scene.create_entity("Box");
    box->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    auto* rb = box->add_component<components::RigidBody2D>();
    rb->mass = 1.0f;
    rb->restitution = 0.0f;
    auto* col = box->add_component<components::BoxCollider2D>();
    col->size = math::Vector2f(1.0f, 1.0f);

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    EXPECT_NEAR(box->transform()->position.y, 1.0f, 0.15f);
    EXPECT_LT(rb->velocity.length(), 0.1f);
}

TEST(Physics2D, RaycastHitsStaticBox) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(4.0f, 1.0f);

    ecs::PhysicsSystem2D sys;
    sys.on_update(scene, 0.01f);

    auto hit = sys.raycast(math::Vector2f(0.0f, 2.0f), math::Vector2f(0.0f, -1.0f), 5.0f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NE(hit->body, physics::k_invalid_body);
    EXPECT_NEAR(hit->point.y, 0.5f, 1e-3f);
    EXPECT_NEAR(hit->normal.y, 1.0f, 1e-3f);
}

TEST(Physics2D, CharacterControllerMovesHorizontally) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    player->add_component<components::RigidBody2D>();
    player->add_component<components::BoxCollider2D>();
    auto* cc = player->add_component<components::CharacterController2D>();
    cc->speed = 4.0f;
    cc->move_input = math::Vector2f(1.0f, 0.0f);

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 120;
    sys.on_update(scene, 0.5f);

    EXPECT_GT(player->transform()->position.x, 0.5f);
    EXPECT_LT(player->transform()->position.y, 2.0f);
}

TEST(Physics2D, DistanceJointConstrainsBodies) {
    scene::Scene scene("test");

    scene::Entity* anchor = scene.create_entity("Anchor");
    anchor->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    anchor->add_component<components::StaticBody2D>();
    anchor->add_component<components::BoxCollider2D>();

    scene::Entity* bob = scene.create_entity("Bob");
    bob->transform()->position = math::Vector3f(2.0f, 0.0f, 0.0f);
    bob->add_component<components::RigidBody2D>();
    bob->add_component<components::BoxCollider2D>();

    scene::Entity* joint_entity = scene.create_entity("Joint");
    auto* joint = joint_entity->add_component<components::Joint2D>();
    joint->body_a_uuid = anchor->uuid();
    joint->body_b_uuid = bob->uuid();
    joint->joint_type = physics::JointType::Distance;
    joint->length = 1.5f;
    joint->anchor_a = math::Vector2f::zero();
    joint->anchor_b = math::Vector2f::zero();

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    math::Vector2f anchor_pos(anchor->transform()->position.x, anchor->transform()->position.y);
    math::Vector2f bob_pos(bob->transform()->position.x, bob->transform()->position.y);
    float dist = (bob_pos - anchor_pos).length();
    EXPECT_NEAR(dist, 1.5f, 0.05f);
}

TEST(Physics2D, CharacterControllerJumpsWhenGrounded) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* player = scene.create_entity("Player");
    // 盒子尺寸 1x1，中心在 0.5 上方时底部刚好接触地面
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody2D>();
    auto* box = player->add_component<components::BoxCollider2D>();
    box->size = math::Vector2f(1.0f, 1.0f);
    auto* cc = player->add_component<components::CharacterController2D>();
    cc->ground_check_distance = 0.55f;
    cc->ground_check_offset = math::Vector2f(0.0f, 0.0f);
    cc->jump_force = 6.0f;
    cc->jump_requested = true;

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 120;
    sys.on_update(scene, 0.1f);

    EXPECT_TRUE(cc->is_grounded);
    auto* rb = player->get_component<components::RigidBody2D>();
    EXPECT_GT(rb->velocity.y, 1.0f);
}

TEST(Physics2D, RaycastMissReturnsNullopt) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(4.0f, 1.0f);

    ecs::PhysicsSystem2D sys;
    sys.on_update(scene, 0.01f);

    auto hit = sys.raycast(math::Vector2f(10.0f, 2.0f), math::Vector2f(0.0f, -1.0f), 5.0f);
    EXPECT_FALSE(hit.has_value());
}

TEST(Physics2D, CharacterControllerPreservesPushVelocity) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody2D>();
    player->add_component<components::BoxCollider2D>();
    auto* cc = player->add_component<components::CharacterController2D>();
    cc->ground_check_distance = 0.55f;
    cc->push_recovery_speed = 2.0f; // 较慢恢复，便于观察保留效果

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 120;
    sys.on_update(scene, 0.05f); // 先落地并接地

    auto* rb = player->get_component<components::RigidBody2D>();
    rb->velocity = math::Vector2f(3.0f, 0.0f);
    sys.on_update(scene, 0.1f);

    // 无输入时，推撞速度应衰减而非立即清零
    EXPECT_GT(rb->velocity.x, 0.5f);
}

TEST(Physics2D, CharacterControllerStepsUpStair) {
    scene::Scene scene("test");

    scene::Entity* ground = scene.create_entity("Ground");
    ground->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    ground->add_component<components::StaticBody2D>();
    auto* ground_col = ground->add_component<components::BoxCollider2D>();
    ground_col->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* step = scene.create_entity("Step");
    step->transform()->position = math::Vector3f(1.5f, 0.4f, 0.0f);
    step->add_component<components::StaticBody2D>();
    auto* step_col = step->add_component<components::BoxCollider2D>();
    step_col->size = math::Vector2f(0.6f, 0.3f);

    scene::Entity* player = scene.create_entity("Player");
    player->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
    player->add_component<components::RigidBody2D>();
    player->add_component<components::BoxCollider2D>();
    auto* cc = player->add_component<components::CharacterController2D>();
    cc->speed = 3.0f;
    cc->ground_check_distance = 0.55f;
    cc->step_height = 0.35f;
    cc->move_input = math::Vector2f(1.0f, 0.0f);

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 1.0f);

    // 角色应越过台阶并到达 x > 1.0
    EXPECT_GT(player->transform()->position.x, 1.0f);
}

TEST(Physics2D, JointDestroyedWhenBodyRemoved) {
    scene::Scene scene("test");

    scene::Entity* anchor = scene.create_entity("Anchor");
    anchor->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
    anchor->add_component<components::StaticBody2D>();
    anchor->add_component<components::BoxCollider2D>();

    scene::Entity* bob = scene.create_entity("Bob");
    bob->transform()->position = math::Vector3f(2.0f, 0.0f, 0.0f);
    bob->add_component<components::RigidBody2D>();
    bob->add_component<components::BoxCollider2D>();

    scene::Entity* joint_entity = scene.create_entity("Joint");
    auto* joint = joint_entity->add_component<components::Joint2D>();
    joint->body_a_uuid = anchor->uuid();
    joint->body_b_uuid = bob->uuid();
    joint->joint_type = physics::JointType::Distance;
    joint->length = 1.5f;

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 120;
    sys.on_update(scene, 0.1f); // 创建关节

    // 销毁 Bob 实体
    scene.destroy_entity(bob);
    sys.on_update(scene, 0.1f); // 应级联销毁关节且不崩溃

    SUCCEED();
}

TEST(Physics2D, TilemapOuterRingColliderAlignedWithTiles) {
    scene::Scene scene("test");

    scene::Entity* level = scene.create_entity("Level");
    level->transform()->position = math::Vector3f(5.0f, 7.0f, 0.0f);
    auto* tm = level->add_component<components::d2::tilemap::Tilemap>(10, 5, 32.0f, 32.0f);
    tm->generate_colliders = true;
    for (int x = 0; x < 10; ++x) {
        tm->set_tile(x, 4, 0); // single solid row at the bottom
    }
    tm->on_init();

    const auto& children = level->children();
    ASSERT_EQ(children.size(), 1u);

    scene::Entity* col_entity = children.front().get();
    auto* box = col_entity->get_component<components::BoxCollider2D>();
    ASSERT_NE(box, nullptr);

    // The merged row spans x=[0,10) and y=[4,5) cells.
    // Collider center must be at the visual center of that block.
    const math::Vector3f& pos = col_entity->transform()->position;
    EXPECT_NEAR(pos.x, 5.0f + (0.0f + 10.0f * 0.5f) * 32.0f, 1e-4f); // 165
    EXPECT_NEAR(pos.y, 7.0f + (4.0f + 0.5f) * 32.0f, 1e-4f);       // 151
    EXPECT_NEAR(box->size.x, 10.0f * 32.0f, 1e-4f);
    EXPECT_NEAR(box->size.y, 32.0f, 1e-4f);
}
