#include "runtime/entity_handle_map.h"

#include <mutex>

#include "scene/scene.h"
#include "scene/entity.h"

namespace gryce_core {

GEntityHandle EntityHandleMap::alloc(const gryce_engine::scene::UUID& uuid) {
    std::unique_lock lock(mutex_);
    int h = next_handle_.fetch_add(1, std::memory_order_relaxed);
    handle_to_uuid_[h] = uuid;
    uuid_to_handle_[uuid] = h;
    return h;
}

void EntityHandleMap::remove(GEntityHandle h) {
    std::unique_lock lock(mutex_);
    auto it = handle_to_uuid_.find(h);
    if (it != handle_to_uuid_.end()) {
        uuid_to_handle_.erase(it->second);
        handle_to_uuid_.erase(it);
    }
}

void EntityHandleMap::clear() {
    std::unique_lock lock(mutex_);
    handle_to_uuid_.clear();
    uuid_to_handle_.clear();
    next_handle_.store(1, std::memory_order_relaxed);
}

void EntityHandleMap::rebuild(gryce_engine::scene::Scene* scene) {
    if (!scene) return;
    clear();
    scene->foreach([this](gryce_engine::scene::Entity* e) {
        if (e) alloc(e->uuid());
    });
}

std::vector<GEntityHandle> EntityHandleMap::all_handles() const {
    std::shared_lock lock(mutex_);
    std::vector<GEntityHandle> out;
    out.reserve(handle_to_uuid_.size());
    for (const auto& p : handle_to_uuid_) {
        out.push_back(p.first);
    }
    return out;
}

gryce_engine::scene::UUID* EntityHandleMap::resolve_uuid(GEntityHandle h) {
    std::shared_lock lock(mutex_);
    auto it = handle_to_uuid_.find(h);
    if (it != handle_to_uuid_.end()) return &it->second;
    return nullptr;
}

bool EntityHandleMap::resolve_uuid_copy(GEntityHandle h, gryce_engine::scene::UUID& out) {
    std::shared_lock lock(mutex_);
    auto it = handle_to_uuid_.find(h);
    if (it == handle_to_uuid_.end()) return false;
    out = it->second;
    return true;
}

GEntityHandle EntityHandleMap::lookup(const gryce_engine::scene::UUID& uuid) {
    std::shared_lock lock(mutex_);
    auto it = uuid_to_handle_.find(uuid);
    if (it != uuid_to_handle_.end()) return it->second;
    return 0;
}

} // namespace gryce_core
