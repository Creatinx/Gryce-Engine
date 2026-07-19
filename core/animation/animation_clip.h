#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.h"

namespace gryce_engine::animation {

// ---------------------------------------------------------------------------
// Keyframe — 单个关键帧。time 单位统一为秒（导入时已完成 ticks → 秒换算）。
// ---------------------------------------------------------------------------
template<typename T>
struct Keyframe {
    float time = 0.0f;
    T value{};
};

// ---------------------------------------------------------------------------
// BoneTrack — 单根骨骼的 P/R/S 三条关键帧轨道。
// 只包含文件中实际动画的通道；缺省通道由采样方回退到 skeleton 的 bind 值。
// ---------------------------------------------------------------------------
struct BoneTrack {
    int32_t bone_index = -1;
    std::vector<Keyframe<math::Vector3f>>    position_keys;
    std::vector<Keyframe<math::Quaternionf>> rotation_keys;
    std::vector<Keyframe<math::Vector3f>>    scale_keys;

    bool empty() const {
        return position_keys.empty() && rotation_keys.empty() && scale_keys.empty();
    }
};

// ---------------------------------------------------------------------------
// AnimationClip — 一段动画剪辑（关键帧集合 + 采样）
// ---------------------------------------------------------------------------
class AnimationClip {
public:
    std::string name;
    // 时长（秒）；0 表示静态/单帧
    float duration = 0.0f;
    // 原始文件的 ticks/sec（导入信息保留，运行时一律用秒）
    float ticks_per_second = 1.0f;

    std::vector<BoneTrack> tracks;

    // 按 bone index 查轨道；无轨道返回 nullptr
    const BoneTrack* find_track(int32_t bone_index) const;

    // 采样指定 bone 在 time（秒）的 local TRS 矩阵。
    // 越界 clamp 到首末关键帧；loop=true 时按 duration 取模（duration<=0 时不循环）。
    // 轨道缺省的通道回退到传入的 bind 值。
    math::Matrix4f sample_bone(int32_t bone_index, float time, bool loop,
                               const math::Vector3f& bind_position,
                               const math::Quaternionf& bind_rotation,
                               const math::Vector3f& bind_scale) const;

    // 单通道采样：keys 为空返回 bind 值；单帧恒等于该帧；多帧二分查找 + 插值。
    // rotation 用 slerp（最短路径），其余线性插值。
    static math::Vector3f sample_vec3(const std::vector<Keyframe<math::Vector3f>>& keys,
                                      float time, const math::Vector3f& bind_value);
    static math::Quaternionf sample_rotation(const std::vector<Keyframe<math::Quaternionf>>& keys,
                                             float time, const math::Quaternionf& bind_value);

    // 时间规整：clamp / loop 取模，供采样与播放共用
    float wrap_time(float time, bool loop) const;
};

} // namespace gryce_engine::animation
