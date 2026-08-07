#include "GryceCore/animator_api.h"
#include "internal_state.h"

#include "assets/skinned_mesh_data.h"
#include "components/skinned_mesh_renderer.h"
#include "scene/entity.h"

#include <cstring>
#include <functional>
#include <string>

using gryce_engine::components::SkinnedMeshRenderer;

namespace {

SkinnedMeshRenderer* find_skinned_renderer(GEntityHandle entity_handle, uint64_t comp_hash) {
    gryce_engine::scene::Entity* e = gryce_core::EntityResolver::resolve(entity_handle);
    if (!e) return nullptr;
    for (const auto& comp : e->components()) {
        std::string type_name = gryce_core::get_component_type_name(comp.get());
        if (std::hash<std::string>{}(type_name) != comp_hash) continue;
        return dynamic_cast<SkinnedMeshRenderer*>(comp.get());
    }
    return nullptr;
}

} // namespace

extern "C" {

int GAnimator_GetClipCount(GEntityHandle entity, uint64_t comp_type_hash) {
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1;
    return static_cast<int>(smr->model()->animations.size());
}

int GAnimator_GetClipName(GEntityHandle entity, uint64_t comp_type_hash,
                          int index, char* out_buf, int buf_size) {
    if (!out_buf || buf_size <= 0) return -1;
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1;
    const auto& clips = smr->model()->animations;
    if (index < 0 || index >= static_cast<int>(clips.size())) return -1;
    std::strncpy(out_buf, clips[static_cast<size_t>(index)].name.c_str(),
                 static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

float GAnimator_GetClipDuration(GEntityHandle entity, uint64_t comp_type_hash, int index) {
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1.0f;
    const auto& clips = smr->model()->animations;
    if (index < 0 || index >= static_cast<int>(clips.size())) return -1.0f;
    return clips[static_cast<size_t>(index)].duration;
}

} // extern "C"
