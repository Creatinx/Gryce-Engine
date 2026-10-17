#include "scene/multi_material_assembly.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "assets/asset_manager.h"
#include "components/mesh_renderer.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

void expand_multi_material_entities(Scene& scene) {
    // 拆分过程会改动层级，先收集目标再处理（避免遍历中修改子节点列表）。
    std::vector<Entity*> targets;
    scene.foreach([&](Entity* entity) {
        if (!entity || !entity->enabled) return;
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr || !mr->enabled || mr->mesh_path.empty()) return;
        if (mr->submesh_index >= 0) return;  // 已拆分 / 场景作者显式指定
        targets.push_back(entity);
    });

    std::size_t expanded = 0;
    for (Entity* entity : targets) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr) continue;

        const std::vector<std::shared_ptr<const assets::MeshData>> submeshes =
            assets::AssetManager::instance().load_meshes(mr->mesh_path);
        if (submeshes.size() <= 1) continue;  // 单子网格：维持既有单网格路径

        const std::string mesh_path = mr->mesh_path;
        const std::string base_name = entity->name();
        const bool billboard = mr->billboard;
        const bool depth_test = mr->depth_test;
        // LOD 调参是"这个模型怎么降级"，不是"这块子网格怎么降级"：场景里写的
        // lod_pixel_error / lod_bias / lod_distances 必须整组下发给子网格，
        // 否则父实体调了预算、子网格还用默认 2px，同一个模型会出现几块降级、
        // 几块不降级的割裂画面（实测：sub0 用 40px 切到 L3，sub1/sub2 仍是 L0）。
        const bool lod_enabled = mr->lod_enabled;
        const float lod_bias = mr->lod_bias;
        const float lod_pixel_error = mr->lod_pixel_error;
        const std::vector<float> lod_distances = mr->lod_distances;

        // 父实体的材质设置当作模板下发：父实体上显式改过的 metallic/roughness/
        // 预设等对全部子网格生效，各子网格自己的贴图仍由文件材质补齐。
        const bool has_template = mr->material != nullptr;

        mr->submesh_index = 0;

        for (std::size_t i = 1; i < submeshes.size(); ++i) {
            auto child = std::make_unique<Entity>(base_name + "_sub" + std::to_string(i));
            // Entity 构造时自带 Transform（局部为单位阵），挂到父实体下后
            // 世界变换与父实体一致 —— 子网格共享同一节点变换。
            auto* child_mr = child->add_component<components::MeshRenderer>(mesh_path);
            child_mr->submesh_index = static_cast<int>(i);
            child_mr->billboard = billboard;
            child_mr->depth_test = depth_test;
            child_mr->lod_enabled = lod_enabled;
            child_mr->lod_bias = lod_bias;
            child_mr->lod_pixel_error = lod_pixel_error;
            child_mr->lod_distances = lod_distances;
            if (has_template) {
                child_mr->ensure_material()->copy_settings_from(*mr->material);
            }
            entity->add_child(std::move(child));
        }
        ++expanded;
        GLOG_INFO("MultiMaterialAssembly: '{}' ({}) split into {} entities",
                  base_name, mesh_path, submeshes.size());
    }

    if (expanded > 0) {
        GLOG_INFO("MultiMaterialAssembly: expanded {} multi-material entity(ies)", expanded);
    }
}

} // namespace gryce_engine::scene
