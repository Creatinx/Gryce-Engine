#pragma once

#include "GryceCore/types.h"
#include "scene/uuid.h"

#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace gryce_engine { namespace scene { class Entity; class Scene; } }

namespace gryce_core {

// ---------------------------------------------------------------------------
// Bidirectional mapping: opaque GEntityHandle (int) <-> internal UUID.
// ---------------------------------------------------------------------------
class EntityHandleMap {
public:
    GEntityHandle alloc(const gryce_engine::scene::UUID& uuid);
    void remove(GEntityHandle h);
    void clear();

    gryce_engine::scene::UUID* resolve_uuid(GEntityHandle h);
    // Copy the UUID under the lock: callers must not hold the returned pointer
    // across a map mutation (insert/erase can rehash and invalidate it).
    bool resolve_uuid_copy(GEntityHandle h, gryce_engine::scene::UUID& out);
    GEntityHandle lookup(const gryce_engine::scene::UUID& uuid);

    // Rebuild entire map from a Scene (e.g. after scene load)
    void rebuild(gryce_engine::scene::Scene* scene);

    // Iterate all valid handles (for GEntity_GetCount / GetAt)
    std::vector<GEntityHandle> all_handles() const;

private:
    std::unordered_map<int, gryce_engine::scene::UUID> handle_to_uuid_;
    std::unordered_map<gryce_engine::scene::UUID, int, std::hash<gryce_engine::scene::UUID>> uuid_to_handle_;
    std::atomic<int> next_handle_{1};
    mutable std::shared_mutex mutex_;
};

} // namespace gryce_core
