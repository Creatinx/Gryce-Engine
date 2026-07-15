#include "common/app_launcher.h"

#include "components/2d/sprite_2d.h"
#include "components/2d/basic_rect.h"
#include "components/2d/light_2d.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Lighting2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Lighting2D"; }

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
        title->transform()->position = math::Vector3f(640.0f, 60.0f, 0.0f);
        title->add_component<components::d2::text::Label>(
            "Lighting2D Demo", 48.0f, render::Color::white());

        // 背景
        scene::Entity* bg = scene.create_entity("Background");
        bg->transform()->position = math::Vector3f(640.0f, 360.0f, -1.0f);
        bg->add_component<components::d2::basic_rect::ColorRect>(1280.0f, 720.0f, render::Color(0.05f, 0.05f, 0.1f, 1.0f));

        // 被照亮的方块墙
        for (int i = 0; i < 5; ++i) {
            scene::Entity* block = scene.create_entity("Block" + std::to_string(i));
            block->transform()->position = math::Vector3f(300.0f + i * 160.0f, 360.0f, 0.0f);
            block->add_component<components::d2::basic_rect::ColorRect>(80.0f, 200.0f, render::Color(0.7f, 0.7f, 0.75f, 1.0f));
        }

        // 动态点光源
        scene::Entity* light = scene.create_entity("PointLight");
        light->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        auto* light_comp = light->add_component<components::d2::light::Light2D>(
            render::Color(1.0f, 0.9f, 0.7f, 1.0f), 1.5f, 350.0f);
        light_comp->light_type = components::d2::light::Light2D::LightType::Point;
        light_entity_ = light;

        // 说明
        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 660.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Move mouse to control the light", 20.0f, render::Color::white());

        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        if (light_entity_) {
            float mx = static_cast<float>(input.mouse_x());
            float my = static_cast<float>(input.mouse_y());
            light_entity_->transform()->position = math::Vector3f(mx, my, 0.0f);
        }
        (void)dt;
        (void)scene;
    }

private:
    scene::Entity* light_entity_ = nullptr;
};

int main(int argc, char* argv[]) {
    Lighting2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
