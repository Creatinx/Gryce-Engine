#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "components/component_factory.h"
#include "components/2d/basic_rect.h"
#include "components/2d/camera_2d.h"
#include "components/subviewport.h"
#include "scene/entity.h"
#include "scene/scene.h"

using namespace gryce_engine;

// Canvas 分层（Godot CanvasLayer 语义）与 Camera2D 旋转/限制测试。
class CanvasLayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
    }
};

TEST_F(CanvasLayerTest, Component2DCanvasLayerSerialization) {
    scene::Scene scene("layers");
    scene::Entity* e = scene.create_entity("Panel");

    auto rect = std::make_unique<components::d2::basic_rect::ColorRect>(100.0f, 50.0f, render::Color::white());
    rect->canvas_layer = 5;
    rect->render_order = 42;
    auto* raw = e->add_component(std::move(rect));

    nlohmann::json json;
    raw->serialize(json);
    EXPECT_EQ(json["canvas_layer"].get<int>(), 5);
    EXPECT_EQ(json["render_order"].get<int>(), 42);

    auto restored = components::d2::basic_rect::ColorRect(0.0f, 0.0f, render::Color::black());
    restored.deserialize(json);
    EXPECT_EQ(restored.canvas_layer, 5);
    EXPECT_EQ(restored.render_order, 42);
}

TEST_F(CanvasLayerTest, Camera2DRotationAndLimitsSerialization) {
    components::d2::camera::Camera2D cam;
    cam.rotation = 0.75f;
    cam.limit_enabled = true;
    cam.limit_left = -100.0f;
    cam.limit_top = -200.0f;
    cam.limit_right = 300.0f;
    cam.limit_bottom = 400.0f;
    cam.offset = math::Vector2f(10.0f, -5.0f);

    nlohmann::json json;
    cam.serialize(json);
    EXPECT_FLOAT_EQ(json["rotation"].get<float>(), 0.75f);
    EXPECT_EQ(json["limit_enabled"].get<bool>(), true);
    EXPECT_FLOAT_EQ(json["limit_right"].get<float>(), 300.0f);

    components::d2::camera::Camera2D restored;
    restored.deserialize(json);
    EXPECT_FLOAT_EQ(restored.rotation, 0.75f);
    EXPECT_TRUE(restored.limit_enabled);
    EXPECT_FLOAT_EQ(restored.limit_left, -100.0f);
    EXPECT_FLOAT_EQ(restored.limit_top, -200.0f);
    EXPECT_FLOAT_EQ(restored.limit_right, 300.0f);
    EXPECT_FLOAT_EQ(restored.limit_bottom, 400.0f);
    EXPECT_FLOAT_EQ(restored.offset.x, 10.0f);
    EXPECT_FLOAT_EQ(restored.offset.y, -5.0f);
}

TEST_F(CanvasLayerTest, Camera2DCenterClampedByLimits) {
    scene::Scene scene("cam");
    scene::Entity* e = scene.create_entity("Camera");
    e->transform()->position = math::Vector3f(5000.0f, 5000.0f, 0.0f);

    auto cam_ptr = std::make_unique<components::d2::camera::Camera2D>();
    cam_ptr->limit_enabled = true;
    cam_ptr->limit_left = -100.0f;
    cam_ptr->limit_top = -100.0f;
    cam_ptr->limit_right = 100.0f;
    cam_ptr->limit_bottom = 100.0f;
    e->add_component(std::move(cam_ptr));

    const auto* cam = e->get_component<components::d2::camera::Camera2D>();
    ASSERT_NE(cam, nullptr);
    math::Vector2f center = cam->center();
    EXPECT_FLOAT_EQ(center.x, 100.0f);
    EXPECT_FLOAT_EQ(center.y, 100.0f);

    e->transform()->position = math::Vector3f(10.0f, -20.0f, 0.0f);
    center = cam->center();
    EXPECT_FLOAT_EQ(center.x, 10.0f);
    EXPECT_FLOAT_EQ(center.y, -20.0f);
}

TEST_F(CanvasLayerTest, SubViewportSerialization) {
    components::SubViewport vp;
    vp.width = 640;
    vp.height = 360;
    vp.camera_name = "MainCamera";
    vp.sprite_entity_name = "Monitor";

    nlohmann::json json;
    vp.serialize(json);
    EXPECT_EQ(json["width"].get<int>(), 640);
    EXPECT_EQ(json["height"].get<int>(), 360);
    EXPECT_EQ(json["camera_name"].get<std::string>(), "MainCamera");
    EXPECT_EQ(json["sprite_entity_name"].get<std::string>(), "Monitor");

    components::SubViewport restored;
    restored.deserialize(json);
    EXPECT_EQ(restored.width, 640);
    EXPECT_EQ(restored.height, 360);
    EXPECT_EQ(restored.camera_name, "MainCamera");
    EXPECT_EQ(restored.sprite_entity_name, "Monitor");
    EXPECT_FALSE(restored.texture_handle.is_valid()); // 运行时字段不序列化
}
