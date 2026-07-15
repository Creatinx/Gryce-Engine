#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "components/static_body.h"
#include "components/audio_source.h"
#include "components/audio_listener.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"

using namespace gryce_engine;

class Audio3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Audio3D"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(0.0f, 3.0f, 8.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;
        cam_entity->add_component<components::AudioListener>();

        camera_.set_position(math::Vector3f(0.0f, 3.0f, 8.0f));
        camera_.set_yaw(-90.0f);
        camera_.set_pitch(-10.0f);
        camera_.set_aspect(1280.0f / 720.0f);

        scene::Entity* light = scene.create_entity("MainLight");
        auto* lc = light->add_component<components::Light>();
        lc->light_type = components::Light::Type::Directional;
        lc->direction = math::Vector3f(-0.4f, -0.8f, -0.3f).normalized();
        lc->intensity = 2.5f;

        // 地面
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -1.0f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.5f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.25f, 0.3f, 0.25f);
            ground_mr->material->roughness = 0.9f;
        }
        ground->add_component<components::StaticBody>();
        ground->add_component<components::PhysicalMaterial>()->apply_preset("Concrete");

        // 音源立方体
        scene::Entity* emitter = scene.create_entity("AudioEmitter");
        emitter->transform()->position = math::Vector3f(0.0f, 1.5f, 0.0f);
        emitter->transform()->scale = math::Vector3f(0.8f, 0.8f, 0.8f);
        auto* em_mr = emitter->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (em_mr && em_mr->material) {
            em_mr->material->use_albedo_map = false;
            em_mr->material->albedo_color = math::Vector3f(0.9f, 0.6f, 0.2f);
            em_mr->material->metallic = 0.4f;
        }
        auto* as = emitter->add_component<components::AudioSource>();
        as->clip_path = "res:/audio/test_tone.wav";
        as->loop = true;
        as->is_3d = true;
        as->volume = 0.8f;
        as->play_on_awake = true;
        emitter_ = emitter;

        upload_meshes(scene, ctx);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        update_camera(dt, input);

        if (emitter_) {
            float t = static_cast<float>(glfwGetTime());
            emitter_->transform()->position = math::Vector3f(
                std::cos(t * 0.7f) * 4.0f, 1.5f, std::sin(t * 0.7f) * 4.0f);
        }

        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;
    scene::Entity* emitter_ = nullptr;

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
    Audio3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
