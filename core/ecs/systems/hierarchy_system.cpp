#include "hierarchy_system.h"

#include "scene/scene.h"
#include "scene/entity.h"
#include "components/hierarchy_components.h"
#include "components/transform.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void HierarchySystem::on_update(scene::Scene& scene, float dt) {
    (void)dt;

    // 1) 通过 ParentComponent 池批量更新全局变换
    //    当前方案：遍历所有有 ParentComponent 的实体，验证其 parent_id 一致性
    //    未来可扩展为：脏标记传播 + 全局变换惰性计算
    auto parent_pool = scene.component_store().pool<components::ParentComponent>();
    if (parent_pool.empty()) return;

    for (auto* pc : parent_pool) {
        if (!pc || !pc->enabled) continue;
        scene::Entity* owner = static_cast<scene::Entity*>(pc->owner());
        if (!owner || !owner->enabled) continue;

        // ParentComponent 的 parent_id 应与 Entity::parent() 保持一致
        // 若不一致，以 Entity::parent() 为准修正 ParentComponent
        scene::Entity* actual_parent = owner->parent();
        ecs::EntityID expected_id = actual_parent ? actual_parent->id() : ecs::k_invalid_entity;
        if (pc->parent_id != expected_id) {
            pc->parent_id = expected_id;
            GLOG_DEBUG("HierarchySystem: fixed ParentComponent mismatch for '{}'", owner->name());
        }
    }

    // 2) 通过 ChildrenComponent 池验证子实体列表一致性
    auto children_pool = scene.component_store().pool<components::ChildrenComponent>();
    if (children_pool.empty()) return;

    for (auto* cc : children_pool) {
        if (!cc || !cc->enabled) continue;
        scene::Entity* owner = static_cast<scene::Entity*>(cc->owner());
        if (!owner) continue;

        // 同步 ChildrenComponent 的 child_ids 与 Entity::children()
        // 以 Entity::children() 为准
        const auto& actual_children = owner->children();
        cc->child_ids.clear();
        cc->child_ids.reserve(actual_children.size());
        for (const auto& child : actual_children) {
            cc->child_ids.push_back(child->id());
        }
    }
}

} // namespace gryce_engine::ecs