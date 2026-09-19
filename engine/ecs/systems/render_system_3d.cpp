#include "ecs/systems/render_system_3d.h"

#include "render/render_pipeline.h"
#include "render/render_context.h"
#include "render/rendering_server.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/transform.h"
#include "components/light.h"
#include "assets/asset_manager.h"
#include "scene/query.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void RenderSystem3D::on_render(scene::Scene& scene, render::RenderContext& ctx) {
    if (!pipeline_) return;

    // 运行时动态创建的 MeshRenderer（如碎裂碎片）需要异步上传到 GPU。
    // 在主线程收集未上传的 MeshRenderer 与对应 MeshData，推送到渲染线程执行上传。
    struct PendingUpload {
        components::MeshRenderer* mr = nullptr;
        // 持有 shared_ptr 防止 LRU 驱逐/重载在命令执行前释放 MeshData
        std::shared_ptr<const assets::MeshData> data;
        render::RenderContext* ctx = nullptr;
        // 生命周期 token：命令延迟执行，期间组件析构则 token 失效，
        // 回调据此跳过，防止 UAF。注意 present() 会 wait_for_idle 排空所有命令，
        // 实体销毁（flush_deferred_ops）发生在 present 之后，token 为双层防护。
        std::shared_ptr<std::atomic<bool>> token;
    };
    std::vector<PendingUpload> pending;

    foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* /*entity*/, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            auto data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (data) {
                pending.push_back({mr, data, &ctx, mr->alive_token()});
            }
        });

    if (!pending.empty()) {
        // 深拷贝 pending 到 lambda，避免引用在 push_command 后失效；
        // 执行前逐个校验 token，组件已析构的条目直接跳过。
        ctx.push_command([pending](render::IRenderBackend*) {
            for (const auto& p : pending) {
                if (!p.token || !p.token->load(std::memory_order_acquire)) continue;
                p.mr->upload_to_gpu(p.ctx, p.data.get(), true);
            }
        });
    }

    // SkinnedMeshRenderer：CPU 模型数据由 AnimatorSystem（Update 阶段）懒加载；
    // 这里只负责把已就绪但未上传的模型推到渲染线程上传。
    struct PendingSkinnedUpload {
        components::SkinnedMeshRenderer* mr = nullptr;
        render::RenderContext* ctx = nullptr;
        std::shared_ptr<std::atomic<bool>> token;
    };
    std::vector<PendingSkinnedUpload> pending_skinned;

    foreach_with_component<components::SkinnedMeshRenderer>(
        scene,
        [&](scene::Entity* /*entity*/, components::SkinnedMeshRenderer* mr) {
            if (!mr || !mr->enabled || mr->model_path.empty() || mr->gpu_mesh()) return;
            // 模型尚未加载（AnimatorSystem 未跑或加载失败）时本帧跳过
            if (!mr->model()) return;
            pending_skinned.push_back({mr, &ctx, mr->alive_token()});
        });

    if (!pending_skinned.empty()) {
        ctx.push_command([pending_skinned](render::IRenderBackend*) {
            for (const auto& p : pending_skinned) {
                if (!p.token || !p.token->load(std::memory_order_acquire)) continue;
                p.mr->upload_to_gpu(p.ctx, true);
            }
        });
    }

    // 如果存在 RenderingServer，通过它提交场景数据
    if (rs_) {
        // 提交实体渲染数据
        foreach_with_components<components::MeshRenderer, components::Transform>(
            scene,
            [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* transform) {
                if (!mr || !mr->enabled || !entity) return;
                rs_->entity_set_transform(entity->id(), transform->local_matrix());
                rs_->entity_set_visible(entity->id(), true);
            });

        // 提交光源数据
        foreach_with_component<components::Light>(
            scene,
            [&](scene::Entity* entity, components::Light* light) {
                if (!light || !light->enabled || !entity) return;

                // 获取或创建光源 RID
                uint32_t light_id = 0;
                auto it = entity_light_map_.find(entity->id());
                if (it != entity_light_map_.end()) {
                    light_id = it->second;
                } else {
                    // 首次见到该实体，创建光源
                    auto lt = static_cast<render::LightType>(light->light_type);
                    light_id = rs_->light_create(lt);
                    entity_light_map_[entity->id()] = light_id;
                }

                // 更新光源属性
                rs_->light_set_color(light_id, light->color);
                rs_->light_set_param(light_id, render::LightParam::Energy, light->intensity);
                rs_->light_set_param(light_id, render::LightParam::Range, light->range);
                rs_->light_set_param(light_id, render::LightParam::SpotAngle, light->spot_angle);
                rs_->light_set_param(light_id, render::LightParam::SpotSoftness, light->spot_softness);

                // 更新光源变换
                auto* t = entity->get_component<components::Transform>();
                if (t) {
                    rs_->light_set_transform(light_id, t->local_matrix());
                }
            });

        // 设置场景并渲染帧
        rs_->set_scene(&scene);
        rs_->render_frame();
    } else {
        // 回退到旧的 RenderPipeline 方式
        pipeline_->render_scene(scene, ctx);
    }
}

} // namespace gryce_engine::ecs
