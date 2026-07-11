#include "ecs/systems/render_system_3d.h"

#include "render/render_pipeline.h"
#include "render/render_context.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/mesh_renderer.h"
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
    };
    std::vector<PendingUpload> pending;

    foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* /*entity*/, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            const assets::MeshData* data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (data) {
                pending.push_back({mr, data, &ctx});
            }
        });

    if (!pending.empty()) {
        // 深拷贝 pending 到 lambda，避免引用在 push_command 后失效
        ctx.push_command([pending](render::IRenderBackend*) {
            for (const auto& p : pending) {
                p.mr->upload_to_gpu(p.ctx, p.data, true);
            }
        });
    }

    pipeline_->render_scene(scene, ctx);
}

} // namespace gryce_engine::ecs
