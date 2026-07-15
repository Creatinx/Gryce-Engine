#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/destructible_body.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/fracture_system.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"

using namespace gryce_engine;

class FractureDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Fracture"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::FractureSystem>();
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(8.0f, 4.0f, 8.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(8.0f, 4.0f, 8.0f));
        camera_.set_yaw(-135.0f);
        camera_.set_pitch(-15.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        scene::Entity* light = scene.create_entity("MainLight");
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.5f, -0.8f, -0.3f).normalized();
        lc->intensity = 2.5f;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -2.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(30.0f, 0.5f, 30.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.3f, 0.3f, 0.32f);
            ground_mr->material->roughness = 0.95f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::BoxCollider>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        // 可破坏立方体
        scene::Entity* cube = scene.create_entity("DestructibleCube");
        cube->transform()->position = math::Vector3f(0.0f, 8.0f, 0.0f);
        cube->transform()->scale = math::Vector3f(2.0f, 2.0f, 2.0f);
        auto* cube_mr = cube->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (cube_mr && cube_mr->material) {
            cube_mr->material->use_albedo_map = false;
            cube_mr->material->albedo_color = math::Vector3f(0.85f, 0.35f, 0.25f);
            cube_mr->material->roughness = 0.5f;
            cube_mr->material->metallic = 0.1f;
        }
        cube->add_component<components::RigidBody>();
        cube->add_component<components::BoxCollider>();
        auto* db = cube->add_component<components::DestructibleBody>();
        db->fracture_threshold = 8.0f;
        db->explosive_impulse = 3.0f;
        db->segments = math::Vector3i(4, 4, 4);
        db->max_fragments = 64;
        db->fragment_lifetime = 6.0f;
        cube->add_component<components::PhysicalMaterial>()->apply_preset("Brick");

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);
        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }

        if (input.is_key_pressed(GLFW_KEY_R)) {
            // 重置场景：删除碎片，重新创建可破坏立方体
            std::vector<scene::Entity*> to_remove;
            scene.foreach([&](scene::Entity* entity) {
                if (entity->name().find("Fragment") == 0) {
                    to_remove.push_back(entity);
                }
            });
            for (auto* e : to_remove) scene.destroy_entity(e);

            if (auto* cube = scene.find_entity_by_name("DestructibleCube")) {
                cube->transform()->position = math::Vector3f(0.0f, 8.0f, 0.0f);
                cube->transform()->rotation = math::Quaternionf::identity();
                if (auto* rb = cube->get_component<components::RigidBody>()) {
                    rb->velocity = math::Vector3f::zero();
                    rb->acceleration = math::Vector3f::zero();
                }
                if (auto* db = cube->get_component<components::DestructibleBody>()) {
                    db->fractured = false;
                }
            }
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
            out.color = light->color;
            out.intensity = light->intensity;
            out.direction = light->direction.normalized();
            lights.push_back(out);
        });
        if (lights.empty()) {
            lights.push_back({ math::Vector3f(0.0f, -1.0f, 0.0f), math::Vector3f::one(), 1.0f });
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
    FractureDemo demo;
    return examples::run_demo(demo, argc, argv);
}
