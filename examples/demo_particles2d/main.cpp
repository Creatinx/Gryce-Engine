#include "common/app_launcher.h"

#include "components/2d/particle_emitter.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Particles2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Particles2D"; }

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
            "Particles2D Demo", 48.0f, render::Color::white());

        // 持续火焰/火花发射器
        scene::Entity* emitter = scene.create_entity("FireEmitter");
        emitter->transform()->position = math::Vector3f(640.0f, 500.0f, 0.0f);
        auto* pe = emitter->add_component<components::d2::ParticleEmitter2D>();
        pe->emission_rate = 120.0f;
        pe->max_particles = 512;
        pe->lifetime_min = 0.6f;
        pe->lifetime_max = 1.2f;
        pe->velocity_min = 60.0f;
        pe->velocity_max = 140.0f;
        pe->direction_min = -3.14159265f * 0.55f;
        pe->direction_max = -3.14159265f * 0.45f;
        pe->acceleration = math::Vector2f(0.0f, -30.0f);
        pe->start_color = render::Color(1.0f, 0.6f, 0.1f, 1.0f);
        pe->end_color = render::Color(0.3f, 0.05f, 0.0f, 0.0f);
        pe->start_size = 14.0f;
        pe->end_size = 2.0f;
        pe->angular_velocity_min = -90.0f;
        pe->angular_velocity_max = 90.0f;
        emitter_ = emitter;

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 660.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Click to burst particles at mouse position", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& input, scene::Scene& scene) override {
        if (emitter_) {
            float mx = static_cast<float>(input.mouse_x());
            float my = static_cast<float>(input.mouse_y());
            emitter_->transform()->position = math::Vector3f(mx, my, 0.0f);

            if (input.is_mouse_button_pressed(0)) {
                auto* pe = emitter_->get_component<components::d2::ParticleEmitter2D>();
                if (pe) pe->burst();
            }
        }
        (void)scene;
    }

private:
    scene::Entity* emitter_ = nullptr;
};

int main(int argc, char* argv[]) {
    Particles2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
