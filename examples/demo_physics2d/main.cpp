#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/2d/basic_rect.h"
#include "components/2d/shape.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Physics2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Physics2D"; }

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
            "Physics2D Demo", 48.0f, render::Color::white());

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(640.0f, 80.0f, 0.0f);
        ground->add_component<components::StaticBody2D>();
        ground->add_component<components::BoxCollider2D>()->size = math::Vector2f(1200.0f, 40.0f);
        auto* ground_rect = ground->add_component<components::d2::basic_rect::ColorRect>(1200.0f, 40.0f, render::Color(0.3f, 0.3f, 0.35f, 1.0f));
        ground_rect->render_order = -1;

        // 斜坡
        scene::Entity* ramp = scene.create_entity("Ramp");
        ramp->transform()->position = math::Vector3f(900.0f, 180.0f, 0.0f);
        ramp->transform()->rotation = math::Quaternionf::from_euler(0.0f, 0.0f, -0.35f);
        ramp->add_component<components::StaticBody2D>();
        ramp->add_component<components::BoxCollider2D>()->size = math::Vector2f(300.0f, 20.0f);
        ramp->add_component<components::d2::basic_rect::ColorRect>(300.0f, 20.0f, render::Color(0.4f, 0.4f, 0.45f, 1.0f));

        // 一堆动态方块和圆
        for (int i = 0; i < 6; ++i) {
            scene::Entity* box = scene.create_entity("Box" + std::to_string(i));
            box->transform()->position = math::Vector3f(350.0f + i * 70.0f, 500.0f + i * 60.0f, 0.0f);
            box->add_component<components::RigidBody2D>();
            box->add_component<components::BoxCollider2D>()->size = math::Vector2f(40.0f, 40.0f);
            box->add_component<components::d2::basic_rect::ColorRect>(40.0f, 40.0f, render::Color(0.9f, 0.5f, 0.3f, 1.0f));
        }

        for (int i = 0; i < 4; ++i) {
            scene::Entity* ball = scene.create_entity("Ball" + std::to_string(i));
            ball->transform()->position = math::Vector3f(450.0f + i * 90.0f, 650.0f + i * 50.0f, 0.0f);
            ball->add_component<components::RigidBody2D>();
            ball->add_component<components::CircleCollider2D>()->radius = 22.0f;
            ball->add_component<components::d2::shape::Circle>(22.0f, 32, render::Color(0.3f, 0.7f, 1.0f, 1.0f));
        }

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 690.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Boxes and circles falling with gravity", 20.0f, render::Color::white());

        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        if (input.is_key_pressed(GLFW_KEY_SPACE)) {
            // 空格生成新方块
            static int spawn_id = 0;
            scene::Entity* box = scene.create_entity("Spawned" + std::to_string(spawn_id++));
            box->transform()->position = math::Vector3f(640.0f, 680.0f, 0.0f);
            box->add_component<components::RigidBody2D>();
            box->add_component<components::BoxCollider2D>()->size = math::Vector2f(35.0f, 35.0f);
            box->add_component<components::d2::basic_rect::ColorRect>(35.0f, 35.0f,
                render::Color(0.2f + static_cast<float>(spawn_id % 7) * 0.12f,
                              0.5f, 0.8f, 1.0f));
        }
        (void)dt;
    }
};

int main(int argc, char* argv[]) {
    Physics2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
