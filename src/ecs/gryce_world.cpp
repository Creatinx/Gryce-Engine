#include "GryceEngineUtils/ecs/world.h"

#include <memory>
#include <vector>

#include "ecs/world.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/mesh_renderer.h"
#include "components/transform.h"
#include "components/light.h"
#include "components/component_factory.h"
#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/storage_rd/light_storage.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"

namespace GryceEngineUtils::ecs {

World::World(Renderer* renderer) : renderer_(renderer) {
    inner_ = std::make_unique<gryce_engine::ecs::World>();
    gryce_engine::components::register_builtin_components();
}

World::~World() = default;

World* World::create(Renderer* renderer) {
    return new World(renderer);
}

void World::destroy() {
    delete this;
}

Entity* World::create_entity(const char* name) {
    if (!inner_) return nullptr;
    if (!inner_->scene()) {
        inner_->attach_scene(std::make_unique<gryce_engine::scene::Scene>("Scene"));
    }
    return inner_->scene()->create_entity(name ? name : "Entity");
}

void World::destroy_entity(Entity* entity) {
    if (inner_ && inner_->scene() && entity) {
        inner_->scene()->destroy_entity(entity);
    }
}

Scene* World::scene() {
    return inner_ ? inner_->scene() : nullptr;
}

void World::update(float dt) {
    if (!inner_) return;
    inner_->update(dt);
    if (!renderer_ || !inner_->scene()) return;

    auto* ctx = renderer_->context();
    if (!ctx) return;

    // 1) 收集尚未上传 GPU 的 MeshRenderer，推送到渲染线程上传（带 alive token）
    struct PendingUpload {
        gryce_engine::components::MeshRenderer* mr = nullptr;
        std::shared_ptr<const gryce_engine::assets::MeshData> data;
        std::shared_ptr<std::atomic<bool>> token;
    };
    std::vector<PendingUpload> pending;
    inner_->scene()->foreach([&](gryce_engine::scene::Entity* e) {
        auto* mr = e->get_component<gryce_engine::components::MeshRenderer>();
        if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
        auto data = gryce_engine::assets::AssetManager::instance().load_mesh(mr->mesh_path);
        if (data) {
            pending.push_back({mr, data, mr->alive_token()});
        }
    });
    if (!pending.empty()) {
        ctx->push_command([pending, ctx](gryce_engine::render::IRenderBackend*) {
            for (const auto& p : pending) {
                if (!p.token || !p.token->load(std::memory_order_acquire)) continue;
                p.mr->upload_to_gpu(ctx, p.data.get(), true);
            }
        });
    }

    // 2) 收集可见物体的 mesh/material/transform，通过 Renderer 直接提交绘制
    inner_->scene()->foreach([&](gryce_engine::scene::Entity* e) {
        auto* mr = e->get_component<gryce_engine::components::MeshRenderer>();
        if (!mr || !mr->enabled || mr->mesh_path.empty()) return;
        auto* gpu = mr->gpu_mesh();
        if (!gpu) return;
        // 组件内部上传的 GPU mesh 需要注册到 Renderer 的指针→句柄映射
        //（register 幂等，可重复调用）
        if (auto* pipeline = renderer_->pipeline()) {
            pipeline->register_mesh_mapping(gpu, mr->gpu_mesh_handle());
        }
        auto* mat = mr->material ? mr->material.get() : nullptr;
        renderer_->draw(gpu, mat, e->world_transform());
    });

    // 3) 收集光源组件 → Renderer::set_lights
    std::vector<LightData> lights;
    inner_->scene()->foreach([&](gryce_engine::scene::Entity* e) {
        auto* light = e->get_component<gryce_engine::components::Light>();
        if (!light || !light->enabled) return;
        LightData ld;
        ld.type = static_cast<gryce_engine::render::LightType>(light->light_type);
        ld.position = e->transform()->position;
        ld.direction = light->direction;
        ld.color = light->color;
        ld.intensity = light->intensity;
        ld.range = light->range;
        ld.spot_angle = light->spot_angle;
        ld.spot_softness = light->spot_softness;
        lights.push_back(ld);
    });
    if (!lights.empty()) {
        renderer_->set_lights(lights.data(), static_cast<int>(lights.size()));
    }
}

} // namespace GryceEngineUtils::ecs
