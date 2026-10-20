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
    // 保留既有映射：同一 UUID 在重建前后沿用原句柄。撤销/重做恢复场景、
    // 场景热重载都会走 rebuild，若一律重新发号，编辑器持有的句柄（选择、
    // 展开状态、属性面板当前目标）会在每次撤销后整片失效。
    // next_handle_ 不重置，因此新实体拿到的一定是未用过的号，不会与
    // 保留句柄撞车。
    std::unordered_map<gryce_engine::scene::UUID, int,
                       std::hash<gryce_engine::scene::UUID>> preserved;
    {
        std::unique_lock lock(mutex_);
        preserved = std::move(uuid_to_handle_);
        handle_to_uuid_.clear();
        uuid_to_handle_.clear();
    }
    scene->foreach([&](gryce_engine::scene::Entity* e) {
        if (!e) return;
        const gryce_engine::scene::UUID uuid = e->uuid();
        auto it = preserved.find(uuid);
        if (it != preserved.end()) {
            std::unique_lock lock(mutex_);
            handle_to_uuid_[it->second] = uuid;
            uuid_to_handle_[uuid] = it->second;
        } else {
            alloc(uuid);
        }
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
