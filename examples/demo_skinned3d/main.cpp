#include "common/app_launcher.h"

#include <GLFW/glfw3.h>

#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "assets/asset_manager.h"
#include "assets/async_loader.h"
#include "ecs/systems/animator_system.h"
#include "ecs/systems/render_system_3d.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/camera.h"
#include "utils/glog/glog_lib.h"

using namespace gryce_engine;

// ---------------------------------------------------------------------------
// Skinned3D Demo — GPU 蒙皮最小演示
// 加载 skinned_triangle.gltf（2 骨骼 + 1 动画 clip），
// AnimatorSystem 每帧求 palette，SkinnedMeshRenderer 走蒙皮 PBR 管线绘制。
// 模型导入走 AsyncLoader 工作线程（load_skinned_model_async），
// GPU 上传由 RenderSystem3D 推送到渲染线程。
// ---------------------------------------------------------------------------
class Skinned3DDemo : public examples::DemoApp {
public:
    const char* title() const override { return "Gryce Demo - Skinned3D"; }
    bool is_3d() const override { return true; }

    void register_systems(ecs::World& world,
                          render::RenderPipeline* pipeline,
                          render::IRenderer2D* /*renderer2d*/) override {
        pipeline_ = pipeline;
        world.add_system<ecs::AnimatorSystem>();
        world.add_system<ecs::RenderSystem3D>(pipeline);
    }

    bool init_scene(scene::Scene& scene, render::RenderContext& ctx) override {
        scene::Entity* cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = math::Vector3f(0.0f, 2.0f, 6.0f);
        auto* cam_comp = cam_entity->add_component<components::Camera>();
        cam_comp->far_plane = 200.0f;
        cam_comp->is_main = true;

        camera_.set_position(math::Vector3f(0.0f, 2.0f, 6.0f));
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

        // 地面（普通 MeshRenderer，验证两条渲染路径共存）
        scene::Entity* ground = scene.create_entity("Ground");
        ground->transform()->position = math::Vector3f(0.0f, -0.5f, 0.0f);
        ground->transform()->scale = math::Vector3f(20.0f, 0.2f, 20.0f);
        auto* ground_mr = ground->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        if (ground_mr && ground_mr->material) {
            ground_mr->material->use_albedo_map = false;
            ground_mr->material->albedo_color = math::Vector3f(0.35f, 0.35f, 0.38f);
            ground_mr->material->roughness = 0.9f;
        }

        // 蒙皮角色：2 骨骼三角形，动画为 joint1 竖直往复平移
        scene::Entity* actor = scene.create_entity("SkinnedActor");
        actor->transform()->position = math::Vector3f(0.0f, 1.0f, 0.0f);
        actor->transform()->scale = math::Vector3f(2.0f, 2.0f, 2.0f);
        auto* smr = actor->add_component<components::SkinnedMeshRenderer>(
            "res:/models/skinned_triangle.gltf");
        if (smr && smr->material) {
            smr->material->use_albedo_map = false;
            smr->material->albedo_color = math::Vector3f(0.9f, 0.5f, 0.2f);
            smr->material->roughness = 0.6f;
        }
        smr->playing = true;
        smr->loop = true;
        skinned_actor_ = actor;

        // 异步导入（纯 CPU，AsyncLoader 工作线程）；完成后 AnimatorSystem
        // 懒加载路径直接从缓存拿到模型。import_skinned 不触碰任何 GPU 对象。
        assets::AssetManager::instance().load_skinned_model_async(
            smr->model_path,
            [](std::shared_ptr<assets::SkinnedModelData> model) {
                if (model) {
                    GLOG_INFO("Skinned3D: async import done ({} bones, {} clips)",
                              model->skeleton.bones.size(), model->animations.size());
                }
            });

        // 地面 mesh 在主线程直接上传（此时渲染线程未启动）
        const assets::MeshData* ground_data =
            assets::AssetManager::instance().load_mesh(ground_mr->mesh_path);
        if (ground_data) ground_mr->upload_to_gpu(&ctx, ground_data);
        return true;
    }

    void update(float dt, platform::InputManager& input, scene::Scene& scene) override {
        // AsyncLoader 完成回调在主线程执行
        assets::AsyncLoader::instance().poll();

        if (input.is_key_pressed(GLFW_KEY_SPACE) && skinned_actor_) {
            auto* smr = skinned_actor_->get_component<components::SkinnedMeshRenderer>();
            if (smr) smr->playing = !smr->playing;
        }

        if (pipeline_) {
            pipeline_->set_camera(camera_);
            pipeline_->set_lights(collect_lights(scene));
        }
    }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    math::Camera camera_;
    scene::Entity* skinned_actor_ = nullptr;

    std::vector<render::RenderPipeline::Light> collect_lights(scene::Scene& scene) {
        std::vector<render::RenderPipeline::Light> lights;
        scene.foreach([&](scene::Entity* entity) {
            auto* light = entity->get_component<components::Light>();
            if (!light || !light->enabled) return;
            render::RenderPipeline::Light out;
            out.type = render::RenderPipeline::LightType::Directional;
            out.direction = light->direction.normalized();
            out.color = light->color;
            out.intensity = light->intensity;
            lights.push_back(out);
        });
        if (lights.empty()) {
            render::RenderPipeline::Light fallback;
            fallback.direction = math::Vector3f(0.0f, -1.0f, 0.0f);
            lights.push_back(fallback);
        }
        return lights;
    }
};

int main(int argc, char* argv[]) {
    Skinned3DDemo demo;
    return examples::run_demo(demo, argc, argv);
}
