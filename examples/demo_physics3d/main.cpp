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

class Physics3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Physics3D"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        // 主摄像机
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(8.0f, 6.0f, 12.0f);
        cam_entity->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(-25.0f), math::to_radians(-35.0f), 0.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->fov = 60.0f;
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(8.0f, 6.0f, 12.0f));
        camera_.set_pitch(-25.0f);
        camera_.set_yaw(-125.0f);
        camera_.set_near_far(0.1f, 200.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        // 主光源
        scene::Entity* light = scene.create_entity("MainLight");
        light->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(-40.0f), math::to_radians(-45.0f), 0.0f);
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.5f, -0.8f, -0.3f).normalized();
        lc->color = math::Vector3f::one();
        lc->intensity = 2.5f;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.5f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.25f, 0.35f, 0.25f);
            ground_mr->material->roughness = 0.9f;
            ground_mr->material->metallic = 0.0f;
        }
        ground->add_component<components::StaticBody>();
        auto* ground_col = ground->add_component<components::BoxCollider>();
        ground_col->size = math::Vector3f(1.0f, 1.0f, 1.0f);
        auto* ground_pm = ground->add_component<components::PhysicalMaterial>();
        ground_pm->apply_preset("Concrete");

        // 堆叠的盒子
        for (int x = 0; x < 3; ++x) {
            for (int y = 0; y < 3; ++y) {
                scene::Entity* box = scene.create_entity("Box_" + std::to_string(x) + "_" + std::to_string(y));
                box->transform()->position = math::Vector3f(
                    static_cast<float>(x) * 1.2f - 1.2f,
                    static_cast<float>(y) * 1.2f + 0.6f,
                    0.0f);
                box->transform()->scale = math::Vector3f(1.0f, 1.0f, 1.0f);
                auto* box_mr = box->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
                if (box_mr && box_mr->material) {
                    box_mr->material->use_albedo_map = false;
                    box_mr->material->albedo_color = math::Vector3f(
                        0.6f + static_cast<float>(x) * 0.15f,
                        0.4f + static_cast<float>(y) * 0.2f,
                        0.5f);
                    box_mr->material->roughness = 0.4f;
                    box_mr->material->metallic = 0.3f;
                }
                box->add_component<components::RigidBody>();
                box->add_component<components::BoxCollider>();
                auto* pm = box->add_component<components::PhysicalMaterial>();
                pm->apply_preset("Wood");
            }
        }

        // 几个球
        for (int i = 0; i < 3; ++i) {
            scene::Entity* ball = scene.create_entity("Ball" + std::to_string(i));
            ball->transform()->position = math::Vector3f(
                static_cast<float>(i) * 1.5f - 1.5f, 8.0f + static_cast<float>(i) * 2.0f, 2.0f);
            ball->transform()->scale = math::Vector3f(0.8f, 0.8f, 0.8f);
            auto* ball_mr = ball->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
            if (ball_mr && ball_mr->material) {
                ball_mr->material->use_albedo_map = false;
                ball_mr->material->albedo_color = math::Vector3f(0.9f, 0.7f, 0.3f);
                ball_mr->material->roughness = 0.2f;
                ball_mr->material->metallic = 0.8f;
            }
            ball->add_component<components::RigidBody>();
            ball->add_component<components::SphereCollider>()->radius = 0.6f;
            auto* pm = ball->add_component<components::PhysicalMaterial>();
            pm->apply_preset("Metal");
        }

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);
        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }

        if (input.is_key_pressed(GLFW_KEY_SPACE)) {
            scene::Entity* ball = scene.create_entity("SpawnedBall");
            ball->transform()->position = camera_.position() + camera_.forward() * 2.0f;
            ball->transform()->scale = math::Vector3f(0.6f, 0.6f, 0.6f);
            ball->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
            auto* rb = ball->add_component<components::RigidBody>();
            rb->velocity = camera_.forward() * 12.0f;
            ball->add_component<components::SphereCollider>()->radius = 0.5f;
            ball->add_component<components::PhysicalMaterial>()->apply_preset("Metal");
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;

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
    Physics3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
