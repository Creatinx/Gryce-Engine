#include "common/app_launcher.h"

#include "components/2d/basic_rect.h"
#include "components/2d/shape.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/joint_2d.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Joints2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Joints2D"; }

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
            "Joints2D Demo", 48.0f, render::Color::white());

        // 天花板
        scene::Entity* ceiling = scene.create_entity("Ceiling");
        ceiling->transform()->position = math::Vector3f(640.0f, 660.0f, 0.0f);
        ceiling->add_component<components::StaticBody2D>();
        ceiling->add_component<components::BoxCollider2D>()->size = math::Vector2f(900.0f, 30.0f);
        ceiling->add_component<components::d2::basic_rect::ColorRect>(900.0f, 30.0f, render::Color(0.4f, 0.4f, 0.45f, 1.0f));

        // 摆球 1
        auto make_pendulum = [&](float x, float y, float length, render::Color color) {
            scene::Entity* pivot = scene.create_entity("Pivot");
            pivot->transform()->position = math::Vector3f(x, y, 0.0f);
            pivot->add_component<components::StaticBody2D>();
            pivot->add_component<components::CircleCollider2D>()->radius = 8.0f;
            pivot->add_component<components::d2::shape::Circle>(8.0f, 16, render::Color::white());

            scene::Entity* ball = scene.create_entity("Ball");
            ball->transform()->position = math::Vector3f(x, y - length, 0.0f);
            ball->add_component<components::RigidBody2D>();
            ball->add_component<components::CircleCollider2D>()->radius = 24.0f;
            ball->add_component<components::d2::shape::Circle>(24.0f, 32, color);

            scene::Entity* joint = scene.create_entity("Joint");
            auto* jc = joint->add_component<components::Joint2D>();
            jc->body_a_uuid = pivot->uuid();
            jc->body_b_uuid = ball->uuid();
            jc->joint_type = physics::JointType::Distance;
            jc->length = length;
            jc->frequency = 2.0f;
            jc->damping = 0.3f;
        };

        make_pendulum(440.0f, 630.0f, 250.0f, render::Color(1.0f, 0.4f, 0.4f, 1.0f));
        make_pendulum(640.0f, 630.0f, 250.0f, render::Color(0.4f, 1.0f, 0.4f, 1.0f));
        make_pendulum(840.0f, 630.0f, 250.0f, render::Color(0.4f, 0.6f, 1.0f, 1.0f));

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 690.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Three pendulums connected by distance joints", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& /*input*/, scene::Scene& /*scene*/) override {
    }
};

int main(int argc, char* argv[]) {
    Joints2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
