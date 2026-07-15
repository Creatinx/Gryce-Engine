#include "common/app_launcher.h"

#include "components/2d/shape.h"
#include "components/2d/camera_2d.h"
#include "components/2d/label.h"
#include "ecs/systems/render_system_2d.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

class Shapes2DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Shapes2D"; }

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
            "Shapes2D Demo", 48.0f, render::Color::white());

        // 圆
        scene::Entity* circle = scene.create_entity("Circle");
        circle->transform()->position = math::Vector3f(320.0f, 280.0f, 0.0f);
        circle->add_component<components::d2::shape::Circle>(80.0f, 64, render::Color(1.0f, 0.4f, 0.4f, 1.0f));

        // 多边形（三角形）
        scene::Entity* tri = scene.create_entity("Triangle");
        tri->transform()->position = math::Vector3f(640.0f, 360.0f, 0.0f);
        std::vector<math::Vector2f> tri_pts = {
            math::Vector2f(0.0f, -100.0f),
            math::Vector2f(90.0f, 80.0f),
            math::Vector2f(-90.0f, 80.0f)
        };
        tri->add_component<components::d2::shape::Polygon>(tri_pts, render::Color(0.4f, 1.0f, 0.4f, 1.0f));

        // 多边形（六边形）
        scene::Entity* hex = scene.create_entity("Hexagon");
        hex->transform()->position = math::Vector3f(960.0f, 280.0f, 0.0f);
        std::vector<math::Vector2f> hex_pts;
        for (int i = 0; i < 6; ++i) {
            float a = static_cast<float>(i) * 3.14159265f / 3.0f;
            hex_pts.emplace_back(std::cos(a) * 80.0f, std::sin(a) * 80.0f);
        }
        hex->add_component<components::d2::shape::Polygon>(hex_pts, render::Color(0.4f, 0.6f, 1.0f, 1.0f));

        scene::Entity* desc = scene.create_entity("Description");
        desc->transform()->position = math::Vector3f(640.0f, 660.0f, 0.0f);
        desc->add_component<components::d2::text::Label>(
            "Circle, triangle and polygon shapes", 20.0f, render::Color::white());

        return true;
    }

    void update(float /*dt*/, platform::InputManager& /*input*/, scene::Scene& /*scene*/) override {
    }
};

int main(int argc, char* argv[]) {
    Shapes2DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
