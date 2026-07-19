#include "animation/pose.h"

namespace gryce_engine::animation {

Pose sample_local_pose(const Skeleton& skeleton, const AnimationClip* clip,
                       float time_seconds, bool loop) {
    Pose local(skeleton.bone_count());
    for (size_t i = 0; i < skeleton.bone_count(); ++i) {
        const Bone& bone = skeleton.bones[i];
        if (clip) {
            local[i] = clip->sample_bone(static_cast<int32_t>(i), time_seconds, loop,
                                         bone.bind_position, bone.bind_rotation, bone.bind_scale);
        } else {
            // 无剪辑：bind pose
            local[i] = bone.bind_local_matrix();
        }
    }
    return local;
}

std::vector<math::Matrix4f> compute_global_pose(const Skeleton& skeleton, const Pose& local) {
    if (local.size() != skeleton.bone_count() || !skeleton.is_valid_hierarchy()) {
        return {};
    }
    std::vector<math::Matrix4f> global(local.size());
    for (size_t i = 0; i < local.size(); ++i) {
        const int32_t parent = skeleton.bones[i].parent_index;
        // 拓扑序保证父节点已求值
        global[i] = (parent < 0) ? local[i] : global[static_cast<size_t>(parent)] * local[i];
    }
    return global;
}

std::vector<math::Matrix4f> compute_skin_palette(const Skeleton& skeleton,
                                                 const std::vector<math::Matrix4f>& global) {
    if (global.size() != skeleton.bone_count()) {
        return {};
    }
    std::vector<math::Matrix4f> palette(global.size());
    for (size_t i = 0; i < global.size(); ++i) {
        palette[i] = global[i] * skeleton.bones[i].inverse_bind_matrix;
    }
    return palette;
}

std::vector<math::Matrix4f> evaluate_skin_palette(const Skeleton& skeleton,
                                                  const AnimationClip* clip,
                                                  float time_seconds, bool loop) {
    Pose local = sample_local_pose(skeleton, clip, time_seconds, loop);
    std::vector<math::Matrix4f> global = compute_global_pose(skeleton, local);
    return compute_skin_palette(skeleton, global);
}

} // namespace gryce_engine::animation
