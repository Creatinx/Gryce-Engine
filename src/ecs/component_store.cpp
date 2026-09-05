#include "component_store.h"

#include "components/component.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void ComponentStore::register_component(EntityID entity, ComponentTypeID type, components::Component* comp) {
    if (!comp || entity == k_invalid_entity) return;

    auto& pool = pools_[type];
    pool.push_back(comp);
    entity_components_[entity].push_back(type);
}

void ComponentStore::unregister_component(EntityID entity, ComponentTypeID type) {
    if (entity == k_invalid_entity) return;

    auto pool_it = pools_.find(type);
    if (pool_it != pools_.end()) {
        auto& pool = pool_it->second;
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if ((*it)->owner() && (*it)->owner()->id() == entity) {
                pool.erase(it);
                break;
            }
        }
        if (pool.empty()) {
            pools_.erase(pool_it);
        }
    }

    auto ec_it = entity_components_.find(entity);
    if (ec_it != entity_components_.end()) {
        auto& types = ec_it->second;
        for (auto it = types.begin(); it != types.end(); ++it) {
            if (*it == type) {
                types.erase(it);
                break;
            }
        }
        if (types.empty()) {
            entity_components_.erase(ec_it);
        }
    }
}

void ComponentStore::unregister_entity(EntityID entity) {
    if (entity == k_invalid_entity) return;

    auto ec_it = entity_components_.find(entity);
    if (ec_it == entity_components_.end()) return;

    for (ComponentTypeID type : ec_it->second) {
        auto pool_it = pools_.find(type);
        if (pool_it == pools_.end()) continue;
        auto& pool = pool_it->second;
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if ((*it)->owner() && (*it)->owner()->id() == entity) {
                pool.erase(it);
                break;
            }
        }
        if (pool.empty()) {
            pools_.erase(pool_it);
        }
    }

    entity_components_.erase(ec_it);
}

bool ComponentStore::has_component(EntityID entity, ComponentTypeID type) const {
    if (entity == k_invalid_entity) return false;

    auto ec_it = entity_components_.find(entity);
    if (ec_it == entity_components_.end()) return false;

    for (ComponentTypeID t : ec_it->second) {
        if (t == type) return true;
    }
    return false;
}

const std::vector<components::Component*>* ComponentStore::pool(ComponentTypeID type) const {
    auto it = pools_.find(type);
    if (it == pools_.end()) return nullptr;
    return &it->second;
}

} // namespace gryce_engine::ecs
