#include <gtest/gtest.h>

#include "components/2d/light_2d.h"
#include "components/2d/sprite_2d.h"
#include "components/2d/tilemap.h"
#include "components/component_factory.h"
#include "render/render2d.h"
#include "resources/project.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Render2DTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
        resources::Project::instance().set_root(std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures");
    }
};

TEST_F(Render2DTest, Light2DSpotSerializeRoundTrip) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("SpotLight");
    auto* light = e->add_component<components::d2::light::Light2D>();
    light->light_type = components::d2::light::Light2D::LightType::Spot;
    light->color = render::Color(1.0f, 0.9f, 0.7f, 1.0f);
    light->intensity = 1.5f;
    light->radius = 250.0f;
    light->range = 500.0f;
    light->direction = math::Vector2f(0.5f, -0.5f);
    light->spot_angle = 40.0f;
    light->spot_softness = 0.3f;

    nlohmann::json json;
    light->serialize(json);

    EXPECT_EQ(json["light_type"].get<std::string>(), "spot");
    EXPECT_FLOAT_EQ(json["intensity"].get<float>(), 1.5f);
    EXPECT_FLOAT_EQ(json["range"].get<float>(), 500.0f);
    EXPECT_FLOAT_EQ(json["spot_angle"].get<float>(), 40.0f);
    EXPECT_FLOAT_EQ(json["spot_softness"].get<float>(), 0.3f);

    auto* loaded = e->add_component<components::d2::light::Light2D>();
    loaded->deserialize(json);
    EXPECT_EQ(loaded->light_type, components::d2::light::Light2D::LightType::Spot);
    EXPECT_FLOAT_EQ(loaded->intensity, 1.5f);
    EXPECT_FLOAT_EQ(loaded->range, 500.0f);
    EXPECT_FLOAT_EQ(loaded->spot_angle, 40.0f);
    EXPECT_FLOAT_EQ(loaded->spot_softness, 0.3f);
}

TEST_F(Render2DTest, Light2DDirectionalSerializeRoundTrip) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("DirLight");
    auto* light = e->add_component<components::d2::light::Light2D>();
    light->light_type = components::d2::light::Light2D::LightType::Directional;
    light->color = render::Color(0.8f, 0.85f, 1.0f, 1.0f);
    light->intensity = 0.6f;
    light->range = 2000.0f;
    light->direction = math::Vector2f(0.2f, -1.0f);

    nlohmann::json json;
    light->serialize(json);

    EXPECT_EQ(json["light_type"].get<std::string>(), "directional");
    EXPECT_FLOAT_EQ(json["intensity"].get<float>(), 0.6f);

    auto* loaded = e->add_component<components::d2::light::Light2D>();
    loaded->deserialize(json);
    EXPECT_EQ(loaded->light_type, components::d2::light::Light2D::LightType::Directional);
    EXPECT_FLOAT_EQ(loaded->intensity, 0.6f);
}

TEST_F(Render2DTest, Sprite2DShadowAndNormalMapSerialize) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("Sprite");
    auto* sprite = e->add_component<components::d2::sprite::Sprite2D>(
        "res:/textures/player.png", 28.0f, 28.0f);
    sprite->normal_map_path = "res:/textures/player_n.png";
    sprite->cast_shadow = true;
    sprite->lit = true;
    sprite->color = render::Color(1.0f, 1.0f, 1.0f, 1.0f);

    nlohmann::json json;
    sprite->serialize(json);

    EXPECT_EQ(json["texture_path"].get<std::string>(), "res:/textures/player.png");
    EXPECT_EQ(json["normal_map_path"].get<std::string>(), "res:/textures/player_n.png");
    EXPECT_TRUE(json["cast_shadow"].get<bool>());
    EXPECT_TRUE(json["lit"].get<bool>());

    auto* loaded = e->add_component<components::d2::sprite::Sprite2D>();
    loaded->deserialize(json);
    EXPECT_EQ(loaded->texture_path, "res:/textures/player.png");
    EXPECT_EQ(loaded->normal_map_path, "res:/textures/player_n.png");
    EXPECT_TRUE(loaded->cast_shadow);
    EXPECT_TRUE(loaded->lit);
}

TEST_F(Render2DTest, TilemapCastShadowSerialize) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("Level");
    auto* tm = e->add_component<components::d2::tilemap::Tilemap>(4, 4, 32.0f, 32.0f);
    tm->cast_shadow = true;
    tm->lit = true;

    nlohmann::json json;
    tm->serialize(json);

    EXPECT_TRUE(json["cast_shadow"].get<bool>());
    EXPECT_TRUE(json["lit"].get<bool>());

    auto* loaded = e->add_component<components::d2::tilemap::Tilemap>();
    loaded->deserialize(json);
    EXPECT_TRUE(loaded->cast_shadow);
    EXPECT_TRUE(loaded->lit);
}

TEST_F(Render2DTest, BloomParamsDefaults) {
    render::BloomParams params;
    EXPECT_FALSE(params.enabled);
    EXPECT_FLOAT_EQ(params.threshold, 1.0f);
    EXPECT_FLOAT_EQ(params.intensity, 0.5f);
    EXPECT_EQ(params.blur_passes, 2);
}
