#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"

using namespace gryce_engine;

class Lighting3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Lighting3D"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(0.0f, 4.0f, 9.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(0.0f, 4.0f, 9.0f));
        camera_.set_yaw(-90.0f);
        camera_.set_pitch(-15.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        // 主方向光
        scene::Entity* dir_light = scene.create_entity("DirectionalLight");
        auto* dlc = dir_light->add_component<components::Light>();
        dlc->light_type = components::Light::Type::Directional;
        dlc->direction = math::Vector3f(-0.4f, -0.85f, -0.35f).normalized();
        dlc->color = math::Vector3f(1.0f, 0.95f, 0.85f);
        dlc->intensity = 2.0f;

        // 点光源 1（红色，左侧）
        scene::Entity* red_light = scene.create_entity("RedPointLight");
        red_light->transform()->position = math::Vector3f(-4.0f, 2.0f, 0.0f);
        auto* rlc = red_light->add_component<components::Light>();
        rlc->light_type = components::Light::Type::Point;
        rlc->color = math::Vector3f(1.0f, 0.2f, 0.15f);
        rlc->intensity = 2.5f;
        rlc->range = 12.0f;
        red_light_ = red_light;

        // 点光源 2（蓝色，右侧）
        scene::Entity* blue_light = scene.create_entity("BluePointLight");
        blue_light->transform()->position = math::Vector3f(4.0f, 2.0f, 0.0f);
        auto* blc = blue_light->add_component<components::Light>();
        blc->light_type = components::Light::Type::Point;
        blc->color = math::Vector3f(0.15f, 0.4f, 1.0f);
        blc->intensity = 2.5f;
        blc->range = 12.0f;
        blue_light_ = blue_light;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.5f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.2f, 0.2f, 0.22f);
            ground_mr->material->roughness = 0.9f;
            ground_mr->material->metallic = 0.0f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::BoxCollider>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        // 中心展示立方体
        scene::Entity* cube = scene.create_entity("ShowcaseCube");
        cube->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
        cube->transform()->scale = math::Vector3f(1.5f, 1.5f, 1.5f);
        cube->add_component<components::RigidBody>();
        cube->add_component<components::BoxCollider>();
        auto* cube_mr = cube->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (cube_mr && cube_mr->material) {
            cube_mr->material->use_albedo_map = false;
            cube_mr->material->albedo_color = math::Vector3f(0.8f, 0.8f, 0.82f);
            cube_mr->material->roughness = 0.3f;
            cube_mr->material->metallic = 0.6f;
        }
        cube->add_component<components::PhysicalMaterial>()->apply_preset("Metal");

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);

        // 让点光源绕中心旋转
        float t = static_cast<float>(glfwGetTime());
        if (red_light_) {
            red_light_->transform()->position = math::Vector3f(
                std::cos(t * 0.8f) * 4.0f, 2.5f, std::sin(t * 0.8f) * 4.0f);
        }
        if (blue_light_) {
            blue_light_->transform()->position = math::Vector3f(
                std::cos(t * 0.6f + 3.14159265f) * 4.0f, 1.5f, std::sin(t * 0.6f + 3.14159265f) * 4.0f);
        }

        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;
    scene::Entity* red_light_ = nullptr;
    scene::Entity* blue_light_ = nullptr;

    void update_camera(float dt, platform::InputManager& input) {
        if (input.is_key_pressed(GLFW_KEY_TAB)) {
            input.set_mouse_locked(!input.is_mouse_locked());
        }
        if (input.is_key_pressed(GLFW_KEY_ESCAPE)) {
            input.set_mouse_locked(false);
        }
        bool mouse_look = input.is_mouse_locked();
        float dx = mouse_look ? static_cast<float>(input.mouse_delta_x()) : 0.0f;
        float dy = mouse_look ? -static_cast<float>(input.mouse_delta_y()) : 0.0f;
        camera_.update(dt,
                       input.is_key_held(GLFW_KEY_W),
                       input.is_key_held(GLFW_KEY_S),
                       input.is_key_held(GLFW_KEY_A),
                       input.is_key_held(GLFW_KEY_D),
                       input.is_key_held(GLFW_KEY_SPACE),
                       input.is_key_held(GLFW_KEY_LEFT_CONTROL),
                       input.is_key_held(GLFW_KEY_LEFT_SHIFT),
                       dx, dy);
    }

    std::vector<render::RenderPipeline::Light> collect_lights(scene::Scene& scene) {
        std::vector<render::RenderPipeline::Light> lights;
        scene.foreach([&](scene::Entity* entity) {
            auto* light = entity->get_component<components::Light>();
            if (!light || !light->enabled) return;
            render::RenderPipeline::Light out;
            switch (light->light_type) {
            case components::Light::Type::Point: out.type = render::RenderPipeline::LightType::Point; break;
            case components::Light::Type::Spot:  out.type = render::RenderPipeline::LightType::Spot;  break;
            default:                             out.type = render::RenderPipeline::LightType::Directional; break;
            }
            out.position = entity->transform() ? entity->transform()->position : math::Vector3f::zero();
            out.direction = light->direction.normalized();
            out.color = light->color;
            out.intensity = light->intensity;
            out.range = light->range;
            out.spot_angle = light->spot_angle;
            lights.push_back(out);
        });
        if (lights.empty()) {
            render::RenderPipeline::Light fallback;
            fallback.direction = math::Vector3f(0.0f, -1.0f, 0.0f);
            lights.push_back(fallback);
        }
        return lights;
    }

    void upload_meshes(scene::Scene& scene, render::RenderContext& ctx) {
        scene.foreach([&](scene::Entity* entity) {
            auto* mr = entity->get_component<components::MeshRenderer>();
            if (!mr || mr->mesh_path.empty()) return;
            const assets::MeshData* data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (data) mr->upload_to_gpu(&ctx, data);
        });
    }
};

int main(int argc, char* argv[]) {
    Lighting3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
