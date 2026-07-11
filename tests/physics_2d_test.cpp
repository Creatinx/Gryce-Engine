#include <gtest/gtest.h>

#ifdef GRYCE_HAS_BOX2D
#include <box2d/box2d.h>
#endif

#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
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
