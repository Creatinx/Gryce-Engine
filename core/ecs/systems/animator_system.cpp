#include "ecs/systems/animator_system.h"

#include "animation/pose.h"
#include "assets/asset_manager.h"
#include "components/skinned_mesh_renderer.h"
#include "ecs/query.h"
#include "render/skinned_vertex.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void AnimatorSystem::on_update(scene::Scene& scene, float dt) {
    foreach_with_component<components::SkinnedMeshRenderer>(
        scene,
        [&](scene::Entity* /*entity*/, components::SkinnedMeshRenderer* mr) {
            if (!mr || !mr->enabled || mr->model_path.empty()) return;

            // 懒加载 CPU 模型数据（AssetManager 缓存，同步路径与 MeshRenderer 一致）
            if (!mr->model()) {
                mr->set_model(assets::AssetManager::instance().load_skinned_model(mr->model_path));
                if (!mr->model()) return; // 加载失败（已告警），下帧重试
            }

            const auto& model = *mr->model();
            const animation::AnimationClip* clip = mr->resolve_clip();

            // 推进播放时间；wrap/clamp 由 AnimationClip 采样层处理
            if (mr->playing && clip) {
                mr->time += dt * mr->speed;
                if (mr->time < 0.0f) mr->time = 0.0f;
            }

            // 求 palette（clip == nullptr → bind pose）。骨骼超上限截断：
            // 顶点 bone id 不会引用被截断的骨（导入侧 bone 数即 palette 大小），
            // 截断只丢弃尾部骨骼的矩阵，超上限模型在加载时已告警。
            std::vector<math::Matrix4f> palette =
                animation::evaluate_skin_palette(model.skeleton, clip, mr->time, mr->loop);
            if (palette.size() > render::k_max_skinning_bones) {
                palette.resize(render::k_max_skinning_bones);
            }
            mr->set_palette(std::make_shared<const std::vector<math::Matrix4f>>(std::move(palette)));
        });
}

} // namespace gryce_engine::ecs
