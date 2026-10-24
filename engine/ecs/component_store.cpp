// ===========================================================================
// engine/ecs/component_store.cpp
// ECS 组件存储池：按"类型视角"分桶登记组件实例。
// ===========================================================================
#include "component_store.h"

#include "components/component.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

// 从指定类型的池里精确摘除一个视角指针。
// 池为空时顺手把桶本身删掉，避免空桶无限堆积。
void erase_view(std::unordered_map<ComponentTypeID, std::vector<void*>, std::hash<std::type_index>>& pools,
                 ComponentTypeID type,
                 void* view) {
    auto pit = pools.find(type);
    if (pit == pools.end()) return;
    auto& bucket = pit->second;
    for (auto it = bucket.begin(); it != bucket.end(); ++it) {
        if (*it == view) {
            bucket.erase(it);
            break;
        }
    }
    if (bucket.empty()) {
        pools.erase(pit);
    }
}

} // namespace

void ComponentStore::register_component(EntityID entity, components::Component* comp) {
    if (!comp || entity == k_invalid_entity) return;

    // 按类型视角展开：自身类型 + 所有祖先类型各登记一份该视角的指针。
    std::vector<components::ComponentTypeView> views;
    comp->collect_type_views(views);
    for (const auto& v : views) {
        pools_[v.type].push_back(v.ptr);
    }
    entity_components_[entity].push_back(comp);
}

void ComponentStore::unregister_component(EntityID entity, components::Component* comp) {
    if (entity == k_invalid_entity || !comp) return;

    // 从实体的组件清单里摘掉这一件（同类型多实例按指针精确匹配）
    auto ec_it = entity_components_.find(entity);
    if (ec_it != entity_components_.end()) {
        auto& list = ec_it->second;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (*it == comp) {
                list.erase(it);
                break;
            }
        }
        if (list.empty()) {
            entity_components_.erase(ec_it);
        }
    }

    // 从它登记过的每个视角池里精确擦除
    std::vector<components::ComponentTypeView> views;
    comp->collect_type_views(views);
    for (const auto& v : views) {
        erase_view(pools_, v.type, v.ptr);
    }
}

void ComponentStore::unregister_entity(EntityID entity) {
    if (entity == k_invalid_entity) return;

    auto ec_it = entity_components_.find(entity);
    if (ec_it == entity_components_.end()) return;

    // 复制一份清单再清理（擦池会改 pools_，但不改 entity_components_，仍先取副本更稳）
    const std::vector<components::Component*> owned = ec_it->second;
    entity_components_.erase(ec_it);

    for (components::Component* comp : owned) {
        if (!comp) continue;
        std::vector<components::ComponentTypeView> views;
        comp->collect_type_views(views);
        for (const auto& v : views) {
            erase_view(pools_, v.type, v.ptr);
        }
    }
}

bool ComponentStore::has_component(EntityID entity, ComponentTypeID type) const {
    if (entity == k_invalid_entity) return false;

    auto ec_it = entity_components_.find(entity);
    if (ec_it == entity_components_.end()) return false;

    for (const components::Component* comp : ec_it->second) {
        if (!comp) continue;
        std::vector<components::ComponentTypeView> views;
        comp->collect_type_views(views);
        for (const auto& v : views) {
            if (v.type == type) return true;
        }
    }
    return false;
}

const std::vector<void*>* ComponentStore::pool(ComponentTypeID type) const {
    auto it = pools_.find(type);
    if (it == pools_.end()) return nullptr;
    return &it->second;
}

} // namespace gryce_engine::ecs
