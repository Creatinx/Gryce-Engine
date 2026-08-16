#include "GryceCore/animator_api.h"
#include "GryceCore/api_guard.h"
#include "runtime/engine_context.h"

#include "assets/skinned_mesh_data.h"
#include "components/skinned_mesh_renderer.h"
#include "scene/entity.h"

#include <cstring>
#include <functional>
#include <string>

using gryce_engine::components::SkinnedMeshRenderer;
using gryce_engine::animation::AnimationClip;
using gryce_engine::animation::BoneTrack;

namespace {

SkinnedMeshRenderer* find_skinned_renderer(GEntityHandle entity_handle, uint64_t comp_hash) {
    GRYCE_API_GUARD();
    gryce_engine::scene::Entity* e = gryce_core::EntityResolver::resolve(entity_handle);
    if (!e) return nullptr;
    for (const auto& comp : e->components()) {
        std::string type_name = gryce_core::get_component_type_name(comp.get());
        if (std::hash<std::string>{}(type_name) != comp_hash) continue;
        return dynamic_cast<SkinnedMeshRenderer*>(comp.get());
    }
    return nullptr;
}

const AnimationClip* find_clip(const SkinnedMeshRenderer* smr, int clip_index) {
    if (!smr || !smr->model()) return nullptr;
    const auto& clips = smr->model()->animations;
    if (clip_index < 0 || clip_index >= static_cast<int>(clips.size())) return nullptr;
    return &clips[static_cast<size_t>(clip_index)];
}

const BoneTrack* find_track(const AnimationClip* clip, int track_index) {
    if (!clip) return nullptr;
    if (track_index < 0 || track_index >= static_cast<int>(clip->tracks.size())) return nullptr;
    return &clip->tracks[static_cast<size_t>(track_index)];
}

} // namespace

extern "C" {

int GAnimator_GetClipCount(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();
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
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1.0f;
    const auto& clips = smr->model()->animations;
    if (index < 0 || index >= static_cast<int>(clips.size())) return -1.0f;
    return clips[static_cast<size_t>(index)].duration;
}

int GAnimator_GetBoneCount(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1;
    return static_cast<int>(smr->model()->skeleton.bones.size());
}

int GAnimator_GetBoneName(GEntityHandle entity, uint64_t comp_type_hash,
                          int bone_index, char* out_buf, int buf_size) {
    if (!out_buf || buf_size <= 0) return -1;
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -1;
    const auto& bones = smr->model()->skeleton.bones;
    if (bone_index < 0 || bone_index >= static_cast<int>(bones.size())) return -1;
    std::strncpy(out_buf, bones[static_cast<size_t>(bone_index)].name.c_str(),
                 static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

int GAnimator_GetBoneParentIndex(GEntityHandle entity, uint64_t comp_type_hash, int bone_index) {
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    if (!smr || !smr->model()) return -2;
    const auto& bones = smr->model()->skeleton.bones;
    if (bone_index < 0 || bone_index >= static_cast<int>(bones.size())) return -2;
    return bones[static_cast<size_t>(bone_index)].parent_index;
}

int GAnimator_GetClipTrackCount(GEntityHandle entity, uint64_t comp_type_hash, int clip_index) {
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    const auto* clip = find_clip(smr, clip_index);
    if (!clip) return -1;
    return static_cast<int>(clip->tracks.size());
}

int GAnimator_GetClipTrackBone(GEntityHandle entity, uint64_t comp_type_hash,
                               int clip_index, int track_index) {
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    const auto* clip = find_clip(smr, clip_index);
    const auto* track = find_track(clip, track_index);
    if (!track) return -1;
    return track->bone_index;
}

int GAnimator_GetClipKeyframeCount(GEntityHandle entity, uint64_t comp_type_hash,
                                   int clip_index, int track_index, int channel) {
    GRYCE_API_GUARD();
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    const auto* clip = find_clip(smr, clip_index);
    const auto* track = find_track(clip, track_index);
    if (!track) return -1;
    switch (channel) {
        case 0: return static_cast<int>(track->position_keys.size());
        case 1: return static_cast<int>(track->rotation_keys.size());
        case 2: return static_cast<int>(track->scale_keys.size());
        default: return -1;
    }
}

int GAnimator_GetClipKeyframe(GEntityHandle entity, uint64_t comp_type_hash,
                              int clip_index, int track_index, int channel,
                              int key_index, float* out, int out_capacity) {
    GRYCE_API_GUARD();
    if (!out || out_capacity < 5) return -1;
    auto* smr = find_skinned_renderer(entity, comp_type_hash);
    const auto* clip = find_clip(smr, clip_index);
    const auto* track = find_track(clip, track_index);
    if (!track) return -1;

    if (channel == 0) {
        if (key_index < 0 || key_index >= static_cast<int>(track->position_keys.size())) return -1;
        const auto& k = track->position_keys[static_cast<size_t>(key_index)];
        out[0] = k.time; out[1] = k.value.x; out[2] = k.value.y; out[3] = k.value.z;
        return 4;
    }
    if (channel == 1) {
        if (key_index < 0 || key_index >= static_cast<int>(track->rotation_keys.size())) return -1;
        const auto& k = track->rotation_keys[static_cast<size_t>(key_index)];
        out[0] = k.time; out[1] = k.value.x; out[2] = k.value.y;
        out[3] = k.value.z; out[4] = k.value.w;
        return 5;
    }
    if (channel == 2) {
        if (key_index < 0 || key_index >= static_cast<int>(track->scale_keys.size())) return -1;
        const auto& k = track->scale_keys[static_cast<size_t>(key_index)];
        out[0] = k.time; out[1] = k.value.x; out[2] = k.value.y; out[3] = k.value.z;
        return 4;
    }
    return -1;
}

} // extern "C"
