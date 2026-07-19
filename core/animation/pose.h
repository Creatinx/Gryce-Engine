#pragma once

#include <vector>

#include "animation/animation_clip.h"
#include "animation/skeleton.h"
#include "math/math.h"

namespace gryce_engine::animation {

// Pose — 每根骨骼的 local TRS 矩阵（下标与 Skeleton::bones 一致）
using Pose = std::vector<math::Matrix4f>;

// 采样 clip 在 time（秒）的 local pose。
// clip == nullptr（或某 bone 无轨道）时对应 bone 回退到 bind TRS。
// loop=true 时时间按 clip duration 取模。
Pose sample_local_pose(const Skeleton& skeleton, const AnimationClip* clip,
                       float time_seconds, bool loop = true);

// local pose → global pose。利用 bones 的拓扑序（父先于子）单趟完成：
// global[root] = local[root]；global[i] = global[parent] * local[i]。
// 层级非法（见 Skeleton::is_valid_hierarchy）时返回空。
std::vector<math::Matrix4f> compute_global_pose(const Skeleton& skeleton, const Pose& local);

// global pose × inverse bind matrix → 蒙皮 palette（skinning 矩阵数组）。
// palette[i] = global[i] * inverse_bind_matrix[i]
std::vector<math::Matrix4f> compute_skin_palette(const Skeleton& skeleton,
                                                 const std::vector<math::Matrix4f>& global);

// 一步便捷接口：采样 → global → palette。clip == nullptr 时输出 bind pose palette
// （数学上恒等于单位阵，可用于验证 skeleton 数据自洽）。
std::vector<math::Matrix4f> evaluate_skin_palette(const Skeleton& skeleton,
                                                  const AnimationClip* clip,
                                                  float time_seconds, bool loop = true);

} // namespace gryce_engine::animation
