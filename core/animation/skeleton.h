#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.h"

namespace gryce_engine::animation {

// ---------------------------------------------------------------------------
// Bone — 单根骨骼
// bones 数组按拓扑序存储（父节点先于子节点），pose 求值可单趟完成。
// ---------------------------------------------------------------------------
struct Bone {
    std::string name;
    // 父骨骼在 bones 中的下标；-1 = 根骨骼
    int32_t parent_index = -1;
    // inverse bind matrix：bind pose 顶点 → bone 空间（蒙皮 palette 的最后一环）
    math::Matrix4f inverse_bind_matrix = math::Matrix4f::identity();
    // bind pose 的 local TRS 分量。动画只替换被动画的通道（如仅 translation），
    // 未动画通道必须回退到这里，因此以分量存储而非仅存的矩阵（矩阵分解不可靠）。
    math::Vector3f bind_position = math::Vector3f::zero();
    math::Quaternionf bind_rotation = math::Quaternionf::identity();
    math::Vector3f bind_scale = math::Vector3f::one();

    // bind local TRS 矩阵（T * R * S）
    math::Matrix4f bind_local_matrix() const;
};

// ---------------------------------------------------------------------------
// Skeleton — 骨架（骨骼列表 + 按名索引）
// ---------------------------------------------------------------------------
class Skeleton {
public:
    std::vector<Bone> bones;

    size_t bone_count() const { return bones.size(); }
    bool empty() const { return bones.empty(); }

    // 按名字查 bone index；未找到返回 -1
    int32_t find_bone(const std::string& name) const;

    // 层级合法性：parent_index ∈ [-1, 自身下标)，保证拓扑序（父先于子）
    bool is_valid_hierarchy() const;
};

} // namespace gryce_engine::animation
