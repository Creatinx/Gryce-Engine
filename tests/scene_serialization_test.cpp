#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "components/2d/label.h"
#include "components/2d/sprite_2d.h"
#include "components/camera.h"
#include "components/component_factory.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "resources/project.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"

using namespace gryce_engine;

// 场景级序列化 round-trip：多种组件（3D 渲染 + 2D 渲染 + 灯光 + 相机）
// 一起经 SceneSerializer 存盘/读回，验证组件工厂重建与字段保真。
class SceneSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
        resources::Project::instance().set_root(std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures");
    }
};

TEST_F(SceneSerializationTest, MultiComponentSceneRoundTrip) {
    scene::Scene scene("roundtrip");

    scene::Entity* cam = scene.create_entity("MainCamera");
    cam->transform()->position = math::Vector3f(1.0f, 2.0f, 3.0f);
    auto* camera = cam->add_component<components::Camera>();
    ASSERT_NE(camera, nullptr);
    camera->fov = 75.0f;
    camera->near_plane = 0.05f;
    camera->far_plane = 500.0f;
    camera->is_main = true;

    scene::Entity* key = scene.create_entity("KeyLight");
    auto* light = key->add_component<components::Light>();
    ASSERT_NE(light, nullptr);
    light->light_type = components::Light::Type::Directional;
    light->color = math::Vector3f(1.0f, 0.9f, 0.8f);
    light->intensity = 3.0f;
    light->range = 40.0f;

    scene::Entity* cube = scene.create_entity("Cube");
    auto* mr = cube->add_component<components::MeshRenderer>("res:/models/cube.obj");
    ASSERT_NE(mr, nullptr);
    mr->billboard = true;

    scene::Entity* banner = scene.create_entity("Banner");
    auto* label = banner->add_component<components::d2::text::Label>(
        "Hello", 32.0f, render::Color(1.0f, 0.2f, 0.2f, 1.0f));
    ASSERT_NE(label, nullptr);

    scene::Entity* star = scene.create_entity("Star");
    auto* sprite = star->add_component<components::d2::sprite::Sprite2D>("res:/textures/star.png", 64.0f, 64.0f);
    ASSERT_NE(sprite, nullptr);
    sprite->cast_shadow = true;

    // 序列化整场
    nlohmann::json json = scene::SceneSerializer::serialize(scene);
    ASSERT_FALSE(json.empty());

    // 反序列化为新场景
    auto restored = scene::SceneSerializer::deserialize(json);
    ASSERT_NE(restored, nullptr);

    // 实体数量与名称保真（含合成根）
    std::vector<std::string> names;
    restored->root()->foreach([&](scene::Entity* e) { names.push_back(e->name()); });
    EXPECT_EQ(names.size(), 6u);  // 合成根 + 5 个实体

    // 逐实体验证关键字段
    scene::Entity* rc = restored->find_entity_by_name("MainCamera");
    ASSERT_NE(rc, nullptr);
    auto* rcam = rc->get_component<components::Camera>();
    ASSERT_NE(rcam, nullptr);
    EXPECT_FLOAT_EQ(rcam->fov, 75.0f);
    EXPECT_FLOAT_EQ(rcam->near_plane, 0.05f);
    EXPECT_TRUE(rcam->is_main);

    scene::Entity* rl = restored->find_entity_by_name("KeyLight");
    ASSERT_NE(rl, nullptr);
    auto* rlight = rl->get_component<components::Light>();
    ASSERT_NE(rlight, nullptr);
    EXPECT_FLOAT_EQ(rlight->intensity, 3.0f);
    EXPECT_FLOAT_EQ(rlight->range, 40.0f);

    scene::Entity* rc2 = restored->find_entity_by_name("Cube");
    ASSERT_NE(rc2, nullptr);
    auto* rmr = rc2->get_component<components::MeshRenderer>();
    ASSERT_NE(rmr, nullptr);
    EXPECT_EQ(rmr->mesh_path, std::string("res:/models/cube.obj"));
    EXPECT_TRUE(rmr->billboard);

    scene::Entity* rb = restored->find_entity_by_name("Banner");
    ASSERT_NE(rb, nullptr);
    auto* rlabel = rb->get_component<components::d2::text::Label>();
    ASSERT_NE(rlabel, nullptr);
    EXPECT_EQ(rlabel->text, std::string("Hello"));

    scene::Entity* rs = restored->find_entity_by_name("Star");
    ASSERT_NE(rs, nullptr);
    auto* rsprite = rs->get_component<components::d2::sprite::Sprite2D>();
    ASSERT_NE(rsprite, nullptr);
    EXPECT_TRUE(rsprite->cast_shadow);
}
