#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/character_controller_3d.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"

using namespace gryce_engine;

class Character3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Character3D"; }
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
        cam_entity->transform()->position = math::Vector3f(0.0f, 3.0f, 8.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(0.0f, 3.0f, 8.0f));
        camera_.set_yaw(-90.0f);
        camera_.set_pitch(-10.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        scene::Entity* light = scene.create_entity("MainLight");
        light->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(-45.0f), math::to_radians(-45.0f), 0.0f);
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.5f, -0.8f, -0.3f).normalized();
        lc->intensity = 2.5f;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -0.5f, 0.0f);
        ground->transform()->scale = math::Vector3f(30.0f, 1.0f, 30.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.3f, 0.35f, 0.3f);
            ground_mr->material->roughness = 0.95f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::BoxCollider>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        // 平台
        for (int i = 0; i < 3; ++i) {
            scene::Entity* plat = scene.create_entity("Platform" + std::to_string(i));
            plat->transform()->position = math::Vector3f(
                static_cast<float>(i) * 4.0f - 4.0f, 1.5f + static_cast<float>(i) * 1.0f, 0.0f);
            plat->transform()->scale = math::Vector3f(3.0f, 0.3f, 3.0f);
            auto* plat_mr = plat->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
            if (plat_mr && plat_mr->material) {
                plat_mr->material->use_albedo_map = false;
                plat_mr->material->albedo_color = math::Vector3f(0.5f, 0.45f, 0.4f);
                plat_mr->material->roughness = 0.8f;
            }
            plat->add_component<components::StaticBody>();
            plat->add_component<components::BoxCollider>();
        }

        // 玩家
        scene::Entity* player = scene.create_entity("Player");
        player->transform()->position = math::Vector3f(0.0f, 2.0f, 0.0f);
        player->transform()->scale = math::Vector3f(0.8f, 1.8f, 0.8f);
        auto* player_mr = player->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (player_mr && player_mr->material) {
            player_mr->material->use_albedo_map = false;
            player_mr->material->albedo_color = math::Vector3f(0.2f, 0.8f, 0.4f);
            player_mr->material->roughness = 0.5f;
        }
        player->add_component<components::RigidBody>();
        player->add_component<components::BoxCollider>();
        auto* cc = player->add_component<components::CharacterController3D>();
        cc->speed = 5.0f;
        cc->jump_force = 8.0f;
        cc->ground_check_distance = 0.2f;
        cc->ground_check_radius = 0.3f;
        player_ = player;

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);
        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }

        if (player_) {
            auto* cc = player_->get_component<components::CharacterController3D>();
            if (cc) {
                math::Vector3f move(0.0f, 0.0f, 0.0f);
                math::Vector3f forward = camera_.forward();
                forward.y = 0.0f;
                forward = forward.normalized();
                math::Vector3f right = camera_.right();
                right.y = 0.0f;
                right = right.normalized();

                if (input.is_key_held(GLFW_KEY_W)) move += forward;
                if (input.is_key_held(GLFW_KEY_S)) move -= forward;
                if (input.is_key_held(GLFW_KEY_A)) move -= right;
                if (input.is_key_held(GLFW_KEY_D)) move += right;

                if (move.length_sq() > 1.0f) move = move.normalized();
                cc->move_input = move;
                cc->jump_requested = input.is_key_pressed(GLFW_KEY_SPACE);

                camera_.set_position(player_->transform()->position + math::Vector3f(0.0f, 1.6f, 0.0f));
            }
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;
    scene::Entity* player_ = nullptr;

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

        camera_.update(dt, false, false, false, false, false, false, false, dx, dy);
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
    Character3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
