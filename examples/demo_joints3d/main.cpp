#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/joint_3d.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"

using namespace gryce_engine;

class Joints3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Joints3D"; }
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
        cam_entity->transform()->position = math::Vector3f(6.0f, 5.0f, 10.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(6.0f, 5.0f, 10.0f));
        camera_.set_yaw(-120.0f);
        camera_.set_pitch(-20.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        scene::Entity* light = scene.create_entity("MainLight");
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.4f, -0.8f, -0.4f).normalized();
        lc->intensity = 2.5f;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -2.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.5f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.25f, 0.3f, 0.35f);
            ground_mr->material->roughness = 0.9f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::BoxCollider>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        // 天花板固定点
        scene::Entity* anchor = scene.create_entity("Anchor");
        anchor->transform()->position = math::Vector3f(0.0f, 6.0f, 0.0f);
        anchor->add_component<components::StaticBody>();
        anchor->add_component<components::BoxCollider>()->size = math::Vector3f(0.5f, 0.5f, 0.5f);
        auto* anchor_mr = anchor->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (anchor_mr && anchor_mr->material) {
            anchor_mr->material->use_albedo_map = false;
            anchor_mr->material->albedo_color = math::Vector3f(0.8f, 0.8f, 0.8f);
        }

        // 链式摆锤
        scene::Entity* prev = anchor;
        for (int i = 0; i < 5; ++i) {
            scene::Entity* link = scene.create_entity("Link" + std::to_string(i));
            link->transform()->position = math::Vector3f(0.0f, 5.0f - static_cast<float>(i) * 1.2f, 0.0f);
            link->transform()->scale = math::Vector3f(0.4f, 0.4f, 0.4f);
            auto* link_mr = link->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
            if (link_mr && link_mr->material) {
                link_mr->material->use_albedo_map = false;
                link_mr->material->albedo_color = math::Vector3f(
                    0.9f - static_cast<float>(i) * 0.15f,
                    0.3f + static_cast<float>(i) * 0.12f,
                    0.4f);
                link_mr->material->metallic = 0.5f;
            }
            link->add_component<components::RigidBody>();
            link->add_component<components::BoxCollider>()->size = math::Vector3f(1.0f, 1.0f, 1.0f);
            link->add_component<components::PhysicalMaterial>()->apply_preset("Metal");

            scene::Entity* joint = scene.create_entity("Joint" + std::to_string(i));
            auto* jc = joint->add_component<components::Joint3D>();
            jc->body_a_uuid = prev->uuid();
            jc->body_b_uuid = link->uuid();
            jc->joint_type = physics::JointType::Hinge;
            jc->anchor_a = math::Vector3f(0.0f, -0.2f, 0.0f);
            jc->anchor_b = math::Vector3f(0.0f, 0.2f, 0.0f);
            jc->axis_a = math::Vector3f(0.0f, 0.0f, 1.0f);
            jc->axis_b = math::Vector3f(0.0f, 0.0f, 1.0f);

            prev = link;
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
    Joints3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
