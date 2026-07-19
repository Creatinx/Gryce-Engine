#include "animation/skeleton.h"

namespace gryce_engine::animation {

math::Matrix4f Bone::bind_local_matrix() const {
    return math::Matrix4f::translate(bind_position) *
           math::Matrix4f::from_quaternion(bind_rotation) *
           math::Matrix4f::scale(bind_scale);
}

int32_t Skeleton::find_bone(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool Skeleton::is_valid_hierarchy() const {
    for (size_t i = 0; i < bones.size(); ++i) {
        const int32_t parent = bones[i].parent_index;
        // 根（-1）合法；其余必须指向已出现的下标（拓扑序：父先于子）
        if (parent < -1 || parent >= static_cast<int32_t>(i)) {
            return false;
        }
    }
    return true;
}

} // namespace gryce_engine::animation
