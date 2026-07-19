#include "ecs/systems/render_system_3d.h"

#include "render/render_pipeline.h"
#include "render/render_context.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "assets/asset_manager.h"
#include "ecs/query.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void RenderSystem3D::on_render(scene::Scene& scene, render::RenderContext& ctx) {
    if (!pipeline_) return;

    // 运行时动态创建的 MeshRenderer（如碎裂碎片）需要异步上传到 GPU。
    // 在主线程收集未上传的 MeshRenderer 与对应 MeshData，推送到渲染线程执行上传。
    struct PendingUpload {
        components::MeshRenderer* mr = nullptr;
        const assets::MeshData* data = nullptr;
        render::RenderContext* ctx = nullptr;
        // 生命周期 token：命令延迟 1~3 帧才在渲染线程执行，
        // 期间组件析构则 token 失效，回调据此跳过，防止 UAF。
        std::shared_ptr<std::atomic<bool>> token;
    };
    std::vector<PendingUpload> pending;

    foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* /*entity*/, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            const assets::MeshData* data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
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
                p.mr->upload_to_gpu(p.ctx, p.data, true);
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

    pipeline_->render_scene(scene, ctx);
}

} // namespace gryce_engine::ecs
