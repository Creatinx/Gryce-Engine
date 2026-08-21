#include <gtest/gtest.h>

#include <memory>

#include "components/component_factory.h"
#include "components/transform.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/static_body_2d.h"
#include "components/rigid_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/3d/colliders.h"
#include "components/3d/navigation_components.h"
#include "components/3d/system_components.h"
#include "components/2d/anim_components.h"
#include "components/2d/physics_components.h"
#include "components/2d/navigation_components.h"
#include "components/common/system_components.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/physics_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

namespace {

const std::vector<std::string> k_new_3d = {
    "Animator", "ParticleSystem3D", "TrailRenderer", "LineRenderer3D", "Decal",
    "Billboard", "TextMesh3D", "Skybox3D", "ReflectionProbe", "LightProbeGroup",
    "FogVolume", "VolumetricLight", "LODGroup", "InstancedMeshRenderer",
    "CapsuleCollider", "CylinderCollider", "ConvexMeshCollider", "MeshCollider",
    "TriggerVolume", "WheelCollider",
    // 3D 寻路与系统级
    "NavigationMesh3D", "NavMeshAgent3D", "NavMeshObstacle3D",
    "RayCast3D", "SpringArm3D", "VisibilityNotifier3D", "AudioReverbZone"
};

const std::vector<std::string> k_new_2d = {
    "AnimatedSprite2D", "NinePatchRect", "LightOccluder2D", "Skeleton2D",
    "Path2D", "PathFollow2D", "PolygonCollider2D", "CapsuleCollider2D",
    "EdgeCollider2D", "TileMapCollider", "Area2D", "RayCast2D",
    "HingeJoint2D", "WeldJoint2D", "PrismaticJoint2D", "WheelJoint2D",
    "RopeJoint2D", "NavigationRegion2D", "NavigationAgent2D", "Marker2D",
    "VisibilityNotifier2D"
};

const std::vector<std::string> k_new_other = { "Timer", "TweenPlayer" };

} // namespace

// ---------------------------------------------------------------------------
// 注册表 / 分类
// ---------------------------------------------------------------------------
TEST(NewComponents, AllTypesRegisteredWithCategories) {
    components::register_builtin_components();
    auto& factory = components::ComponentFactory::instance();

    for (const auto& name : k_new_3d) {
        EXPECT_TRUE(factory.has_type(name)) << name;
        EXPECT_STREQ(factory.category(name), "Node3D") << name;
        EXPECT_TRUE(factory.create(name) != nullptr) << name;
    }
    for (const auto& name : k_new_2d) {
        EXPECT_TRUE(factory.has_type(name)) << name;
        EXPECT_STREQ(factory.category(name), "Node2D") << name;
        EXPECT_TRUE(factory.create(name) != nullptr) << name;
    }
    for (const auto& name : k_new_other) {
        EXPECT_TRUE(factory.has_type(name)) << name;
        EXPECT_STREQ(factory.category(name), "Other") << name;
        EXPECT_TRUE(factory.create(name) != nullptr) << name;
    }
}

// ---------------------------------------------------------------------------
// 序列化往返
// ---------------------------------------------------------------------------
TEST(NewComponents, SerializeRoundtrip) {
    components::register_builtin_components();
    auto& factory = components::ComponentFactory::instance();

    auto capsule = factory.create("CapsuleCollider");
    ASSERT_TRUE(capsule);
    auto* cc = static_cast<components::CapsuleCollider*>(capsule.get());
    cc->radius = 0.75f;
    cc->height = 2.5f;
    cc->center = math::Vector3f(1.0f, 2.0f, 3.0f);
    cc->is_trigger = true;
    nlohmann::json j;
    capsule->serialize(j);
    auto clone = factory.create("CapsuleCollider");
    clone->deserialize(j);
    auto* c2 = static_cast<components::CapsuleCollider*>(clone.get());
    EXPECT_FLOAT_EQ(c2->radius, 0.75f);
    EXPECT_FLOAT_EQ(c2->height, 2.5f);
    EXPECT_EQ(c2->center, math::Vector3f(1.0f, 2.0f, 3.0f));
    EXPECT_TRUE(c2->is_trigger);

    auto path = factory.create("Path2D");
    ASSERT_TRUE(path);
    auto* p = static_cast<components::d2::Path2D*>(path.get());
    p->points = { math::Vector2f(0, 0), math::Vector2f(5, 0), math::Vector2f(5, 5) };
    p->closed = true;
    nlohmann::json pj;
    path->serialize(pj);
    auto pclone = factory.create("Path2D");
    pclone->deserialize(pj);
    auto* p2 = static_cast<components::d2::Path2D*>(pclone.get());
    ASSERT_EQ(p2->points.size(), 3u);
    EXPECT_EQ(p2->points[2], math::Vector2f(5, 5));
    EXPECT_TRUE(p2->closed);
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------
TEST(NewComponents, TimerFiresAndStops) {
    scene::Scene scene("timer");
    scene::Entity* e = scene.create_entity("T");
    auto* timer = e->add_component<components::Timer>();
    timer->wait_time = 0.5f;
    timer->auto_start = true;
    timer->one_shot = true;
    timer->reset(); // on_awake 已用默认值启动，重新按新 wait_time 计时

    scene.update(0.6f);
    EXPECT_EQ(timer->timeout_count, 1);
    EXPECT_TRUE(timer->is_finished);
    EXPECT_FALSE(timer->is_running);

    scene.update(1.0f);
    EXPECT_EQ(timer->timeout_count, 1); // one_shot 不再触发
}

// ---------------------------------------------------------------------------
// TweenPlayer
// ---------------------------------------------------------------------------
TEST(NewComponents, TweenPlayerMovesTransform) {
    scene::Scene scene("tween");
    scene::Entity* e = scene.create_entity("Tween");
    e->transform()->position = math::Vector3f::zero();
    auto* tween = e->add_component<components::TweenPlayer>();
    tween->from = math::Vector3f::zero();
    tween->to = math::Vector3f(10.0f, 0.0f, 0.0f);
    tween->duration = 1.0f;
    tween->playing = true;

    scene.update(0.5f);
    EXPECT_NEAR(e->transform()->position.x, 5.0f, 0.2f);

    scene.update(0.6f);
    EXPECT_NEAR(e->transform()->position.x, 10.0f, 0.01f);
    EXPECT_FALSE(tween->playing);
}

// ---------------------------------------------------------------------------
// PathFollow2D
// ---------------------------------------------------------------------------
TEST(NewComponents, PathFollow2DMovesAlongParentPath) {
    scene::Scene scene("path");
    scene::Entity* path_entity = scene.create_entity("Path");
    auto* path = path_entity->add_component<components::d2::Path2D>();
    path->points = { math::Vector2f(0, 0), math::Vector2f(10, 0) };

    auto child = std::make_unique<scene::Entity>("Follower");
    scene::Entity* follower = path_entity->add_child(std::move(child));
    auto* follow = follower->add_component<components::d2::PathFollow2D>();
    follow->speed = 0.5f; // 0.5 progress/s → 半程 1s

    scene.update(0.5f);
    EXPECT_NEAR(follower->transform()->position.x, 2.5f, 0.05f);
    scene.update(0.5f);
    EXPECT_NEAR(follower->transform()->position.x, 5.0f, 0.05f);
}

// ---------------------------------------------------------------------------
// NavMeshAgent3D / NavigationAgent2D（直线路径占位实现）
// ---------------------------------------------------------------------------
TEST(NewComponents, NavMeshAgent3DMovesToTarget) {
    scene::Scene scene("nav3d");
    scene::Entity* e = scene.create_entity("Agent");
    auto* agent = e->add_component<components::NavMeshAgent3D>();
    agent->speed = 2.0f;
    agent->set_target(math::Vector3f(4.0f, 0.0f, 0.0f));

    scene.update(1.0f);
    EXPECT_NEAR(e->transform()->position.x, 2.0f, 0.1f);
    scene.update(2.0f);
    EXPECT_NEAR(e->transform()->position.x, 4.0f, 0.1f);
    EXPECT_FALSE(agent->path_following);
}

TEST(NewComponents, NavigationAgent2DMovesToTarget) {
    scene::Scene scene("nav2d");
    scene::Entity* e = scene.create_entity("Agent");
    auto* agent = e->add_component<components::d2::NavigationAgent2D>();
    agent->speed = 2.0f;
    agent->set_target(math::Vector2f(4.0f, 0.0f));

    scene.update(1.0f);
    EXPECT_NEAR(e->transform()->position.x, 2.0f, 0.1f);
    scene.update(2.0f);
    EXPECT_NEAR(e->transform()->position.x, 4.0f, 0.1f);
    EXPECT_FALSE(agent->path_following);
}

// ---------------------------------------------------------------------------
// 3D 新碰撞体 + RayCast3D
// ---------------------------------------------------------------------------
TEST(NewComponents, CapsuleRestsOnBox) {
    scene::Scene scene("capsule3d");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody>();
    auto* gc = ground->add_component<components::BoxCollider>();
    gc->size = math::Vector3f(4.0f, 1.0f, 4.0f);

    scene::Entity* body = scene.create_entity("Capsule");
    body->transform()->position = math::Vector3f(0.0f, 3.0f, 0.0f);
    auto* rb = body->add_component<components::RigidBody>();
    rb->mass = 1.0f;
    auto* cap = body->add_component<components::CapsuleCollider>();
    cap->radius = 0.5f;
    cap->height = 2.0f;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    EXPECT_GT(body->transform()->position.y, 1.3f);
    EXPECT_LT(body->transform()->position.y, 1.7f);
}

TEST(NewComponents, CylinderRestsOnBox) {
    scene::Scene scene("cylinder3d");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody>();
    auto* gc = ground->add_component<components::BoxCollider>();
    gc->size = math::Vector3f(4.0f, 1.0f, 4.0f);

    scene::Entity* body = scene.create_entity("Cylinder");
    body->transform()->position = math::Vector3f(0.0f, 3.0f, 0.0f);
    auto* rb = body->add_component<components::RigidBody>();
    rb->mass = 1.0f;
    auto* cyl = body->add_component<components::CylinderCollider>();
    cyl->radius = 0.5f;
    cyl->height = 1.0f;

    ecs::PhysicsSystem3D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    EXPECT_GT(body->transform()->position.y, 0.8f);
    EXPECT_LT(body->transform()->position.y, 1.2f);
}

TEST(NewComponents, RayCast3DFillsComponent) {
    scene::Scene scene("ray3d");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody>();
    auto* gc = ground->add_component<components::BoxCollider>();
    gc->size = math::Vector3f(4.0f, 1.0f, 4.0f);

    scene::Entity* probe = scene.create_entity("Probe");
    probe->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
    auto* ray = probe->add_component<components::RayCast3D>();
    ray->direction = math::Vector3f(0.0f, -1.0f, 0.0f);
    ray->max_distance = 10.0f;

    ecs::PhysicsSystem3D sys;
    sys.on_update(scene, 0.01f);

    EXPECT_TRUE(ray->hit);
    EXPECT_NEAR(ray->hit_distance, 1.5f, 1e-3f);
    EXPECT_NEAR(ray->hit_point.y, 0.5f, 1e-3f);
}

// ---------------------------------------------------------------------------
// 2D 新碰撞体 + RayCast2D
// ---------------------------------------------------------------------------
TEST(NewComponents, PolygonAndCapsuleRestOnGround) {
    scene::Scene scene("poly2d");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody2D>();
    auto* gc = ground->add_component<components::BoxCollider2D>();
    gc->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* poly = scene.create_entity("Poly");
    poly->transform()->position = math::Vector3f(-1.5f, 3.0f, 0.0f);
    auto* prb = poly->add_component<components::RigidBody2D>();
    prb->mass = 1.0f;
    poly->add_component<components::d2::PolygonCollider2D>();

    scene::Entity* cap = scene.create_entity("Capsule");
    cap->transform()->position = math::Vector3f(1.5f, 3.0f, 0.0f);
    auto* crb = cap->add_component<components::RigidBody2D>();
    crb->mass = 1.0f;
    auto* cc = cap->add_component<components::d2::CapsuleCollider2D>();
    cc->radius = 0.25f;
    cc->height = 1.0f;

    ecs::PhysicsSystem2D sys;
    sys.max_steps_per_frame = 240;
    sys.on_update(scene, 2.0f);

    EXPECT_GT(poly->transform()->position.y, 0.8f);
    EXPECT_LT(poly->transform()->position.y, 1.2f);
    EXPECT_GT(cap->transform()->position.y, 0.9f);
    EXPECT_LT(cap->transform()->position.y, 1.3f);
}

TEST(NewComponents, RayCast2DFillsComponent) {
    scene::Scene scene("ray2d");
    scene::Entity* ground = scene.create_entity("Ground");
    ground->add_component<components::StaticBody2D>();
    auto* gc = ground->add_component<components::BoxCollider2D>();
    gc->size = math::Vector2f(10.0f, 1.0f);

    scene::Entity* probe = scene.create_entity("Probe");
    probe->transform()->position = math::Vector3f(0.0f, 5.0f, 0.0f);
    auto* ray = probe->add_component<components::d2::RayCast2D>();
    ray->direction = math::Vector2f(0.0f, -1.0f);
    ray->max_distance = 10.0f;

    ecs::PhysicsSystem2D sys;
    sys.on_update(scene, 0.01f);

    EXPECT_TRUE(ray->hit);
    EXPECT_NEAR(ray->hit_distance, 4.5f, 1e-3f);
    EXPECT_NEAR(ray->hit_point.y, 0.5f, 1e-3f);
}
