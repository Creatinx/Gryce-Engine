#include "common/app_launcher.h"

#include "components/2d/tilemap.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Tilemap2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Tilemap2D"; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* /*pipeline*/,
                          render::IRenderer2D* renderer2d) override {
        world.add_system<ecs::RenderSystem2D>(renderer2d);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& /*ctx*/) override {
        scene::Entity* cam = scene.create_entity("Camera");
        cam->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        cam->add_component<components::d2::camera::Camera2D>();

        scene::Entity* title = scene.create_entity("Title");
        title->transform()->position = math::Vector3f(640.0f, 40.0f, 0.0f);
        title->add_component<components::d2::text::Label>(
            "Tilemap2D Demo", 48.0f, render::Color::white());

        // 20x15 的地图，格子 32x32，居中显示
        scene::Entity* map = scene.create_entity("Tilemap");
        map->transform()->position = math::Vector3f(240.0f, 120.0f, 0.0f);
        auto* tilemap = map->add_component<components::d2::tilemap::Tilemap>(24, 15, 32.0f, 32.0f);
        tilemap->use_tileset_texture = false;
        tilemap->lit = false;

        // 地面
        for (int x = 0; x < 24; ++x) {
            tilemap->set_tile(x, 13, 1);
            tilemap->set_tile(x, 14, 1);
        }
        // 平台
        for (int x = 4; x < 10; ++x) tilemap->set_tile(x, 9, 2);
        for (int x = 14; x < 20; ++x) tilemap->set_tile(x, 6, 2);
        // 一些装饰
        tilemap->set_tile(2, 12, 3);
        tilemap->set_tile(21, 12, 3);
        tilemap->set_tile(12, 4, 0);

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 690.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Procedural tilemap with colored tiles", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& /*input*/, scene::Scene& /*scene*/) override {
    }
};

int main(int argc, char* argv[]) {
    Tilemap2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
