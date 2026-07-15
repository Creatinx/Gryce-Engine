#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/2d/basic_rect.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/character_controller_2d.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Character2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Character2D"; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* /*pipeline*/,
                          render::IRenderer2D* renderer2d) override {
        world.add_system<ecs::PhysicsSystem2D>();
        world.add_system<ecs::RenderSystem2D>(renderer2d);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& /*ctx*/) override {
        scene::Entity* cam = scene.create_entity("Camera");
        cam->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        cam->add_component<components::d2::camera::Camera2D>();

        scene::Entity* title = scene.create_entity("Title");
        title->transform()->position = math::Vector3f(640.0f, 40.0f, 0.0f);
        title->add_component<components::d2::text::Label>(
            "Character2D Demo", 48.0f, render::Color::white());

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(640.0f, 80.0f, 0.0f);
        ground->add_component<components::StaticBody2D>();
        ground->add_component<components::BoxCollider2D>()->size = math::Vector2f(1200.0f, 40.0f);
        ground->add_component<components::d2::basic_rect::ColorRect>(1200.0f, 40.0f, render::Color(0.3f, 0.3f, 0.35f, 1.0f));

        // 平台
        for (int i = 0; i < 3; ++i) {
            scene::Entity* plat = scene.create_entity("Platform" + std::to_string(i));
            plat->transform()->position = math::Vector3f(300.0f + i * 260.0f, 240.0f + i * 120.0f, 0.0f);
            plat->add_component<components::StaticBody2D>();
            plat->add_component<components::BoxCollider2D>()->size = math::Vector2f(200.0f, 20.0f);
            plat->add_component<components::d2::basic_rect::ColorRect>(200.0f, 20.0f, render::Color(0.4f, 0.4f, 0.45f, 1.0f));
        }

        // 玩家
        scene::Entity* player = scene.create_entity("Player");
        player->transform()->position = math::Vector3f(200.0f, 300.0f, 0.0f);
        player->add_component<components::RigidBody2D>();
        player->add_component<components::BoxCollider2D>()->size = math::Vector2f(40.0f, 60.0f);
        auto* cc = player->add_component<components::CharacterController2D>();
        cc->speed = 250.0f;
        cc->jump_force = 550.0f;
        cc->ground_check_distance = 35.0f;
        cc->ground_check_span = 15.0f;
        player->add_component<components::d2::basic_rect::ColorRect>(40.0f, 60.0f, render::Color(0.2f, 0.9f, 0.4f, 1.0f));
        player_ = player;

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 690.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "A/D move, Space jump", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& input, scene::Scene& /*scene*/) override {
        if (!player_) return;
        auto* cc = player_->get_component<components::CharacterController2D>();
        if (!cc) return;

        math::Vector2f move(0.0f, 0.0f);
        if (input.is_key_held(GLFW_KEY_A) || input.is_key_held(GLFW_KEY_LEFT)) move.x -= 1.0f;
        if (input.is_key_held(GLFW_KEY_D) || input.is_key_held(GLFW_KEY_RIGHT)) move.x += 1.0f;
        cc->move_input = move;
        cc->jump_requested = input.is_key_pressed(GLFW_KEY_SPACE);
    }

private:
    scene::Entity* player_ = nullptr;
};

int main(int argc, char* argv[]) {
    Character2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
