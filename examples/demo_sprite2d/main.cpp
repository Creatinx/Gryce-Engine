#include "common/app_launcher.h"

#include "components/2d/sprite_2d.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Sprite2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Sprite2D"; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* /*pipeline*/,
                          render::IRenderer2D* renderer2d) override {
        world.add_system<ecs::RenderSystem2D>(renderer2d);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& /*ctx*/) override {
        // 创建主摄像机
        scene::Entity* cam = scene.create_entity("Camera");
        cam->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        cam->add_component<components::d2::camera::Camera2D>();

        // 标题文字
        scene::Entity* title = scene.create_entity("Title");
        title->transform()->position = math::Vector3f(640.0f, 80.0f, 0.0f);
        title->add_component<components::d2::text::Label>(
            "Sprite2D Demo", 48.0f, render::Color::white());

        // 带贴图的精灵
        scene::Entity* textured = scene.create_entity("TexturedSprite");
        textured->transform()->position = math::Vector3f(440.0f, 360.0f, 0.0f);
        auto* sprite = textured->add_component<components::d2::sprite::Sprite2D>(
            "res:/textures/player.png", 128.0f, 128.0f);
        sprite->color = render::Color::white();
        sprite->lit = false;

        // 纯色精灵（无贴图）
        scene::Entity* colored = scene.create_entity("ColoredSprite");
        colored->transform()->position = math::Vector3f(840.0f, 360.0f, 0.0f);
        auto* colored_sprite = colored->add_component<components::d2::sprite::Sprite2D>("", 128.0f, 128.0f);
        colored_sprite->color = render::Color(0.2f, 0.7f, 1.0f, 1.0f);
        colored_sprite->lit = false;

        // 说明文字
        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 620.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Left: textured sprite | Right: solid color sprite", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& /*input*/, scene::Scene& /*scene*/) override {
    }
};

int main(int argc, char* argv[]) {
    Sprite2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
