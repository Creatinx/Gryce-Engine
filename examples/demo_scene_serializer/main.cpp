#include "common/app_launcher.h"

#include <GLFW/glfw3.h>
#include <filesystem>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/entity.h"
#include "math/camera.h"
#include "resources/project.h"

using namespace gryce_engine;

class SceneSerializerDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - SceneSerializer"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        scene_path_ = resources::Project::instance().root() + "/scenes/serializer_test.gesc";

        if (std::filesystem::exists(scene_path_)) {
            GLOG_INFO("Loading existing serialized scene from {}", scene_path_);
            auto loaded = scene::SceneSerializer::load_from_file("res:/scenes/serializer_test.gesc");
            if (loaded) {
                // 将加载的场景内容合并到当前 scene：直接替换所有实体
                std::vector<scene::Entity*> to_remove;
                scene.foreach([&](scene::Entity* e) { to_remove.push_back(e); });
                for (auto* e : to_remove) scene.destroy_entity(e);
                for (const auto& root : loaded->roots()) {
                    scene.add_root_entity(root->clone());
                }
            }
        } else {
            GLOG_INFO("Creating new demo scene and saving to {}", scene_path_);
            create_demo_content(scene);
            std::filesystem::create_directories(std::filesystem::path(scene_path_).parent_path());
            scene::SceneSerializer::save_to_file(scene, "res:/scenes/serializer_test.gesc");
        }

        camera_.set_position(math::Vector3f(6.0f, 5.0f, 10.0f));
        camera_.set_yaw(-120.0f);
        camera_.set_pitch(-20.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);
        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }

        if (input.is_key_pressed(GLFW_KEY_S)) {
            GLOG_INFO("Saving scene to {}", scene_path_);
            scene::SceneSerializer::save_to_file(scene, "res:/scenes/serializer_test.gesc");
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;
    std::string scene_path_;

    void create_demo_content(scene::Scene& scene) {
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(6.0f, 5.0f, 10.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        scene::Entity* light = scene.create_entity("MainLight");
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.5f, -0.8f, -0.3f).normalized();
        lc->intensity = 2.5f;

        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.5f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.25f, 0.3f, 0.28f);
            ground_mr->material->roughness = 0.9f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::BoxCollider>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        scene::Entity* cube = scene.create_entity("SerializedCube");
        cube->transform()->position = math::Vector3f(0.0f, 3.0f, 0.0f);
        cube->transform()->scale = math::Vector3f(1.5f, 1.5f, 1.5f);
        auto* cube_mr = cube->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (cube_mr && cube_mr->material) {
            cube_mr->material->use_albedo_map = false;
            cube_mr->material->albedo_color = math::Vector3f(0.4f, 0.6f, 0.9f);
            cube_mr->material->roughness = 0.4f;
            cube_mr->material->metallic = 0.4f;
        }
        cube->add_component<components::RigidBody>();
        cube->add_component<components::BoxCollider>();
        cube->add_component<components::PhysicalMaterial>()->apply_preset("Wood");
    }

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
    SceneSerializerDemo demo;
    return examples::run_demo(demo, argc, argv);
}
