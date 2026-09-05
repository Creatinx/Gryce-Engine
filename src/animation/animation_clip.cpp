#include "animation/animation_clip.h"

#include <algorithm>
#include <cmath>

namespace gryce_engine::animation {

namespace {

// 在按 time 升序的关键帧序列中定位 time 所在区间 [i, i+1]，并给出插值系数。
// 返回值：左端下标；t 越界时由调用方先行 clamp，此处假定 keys 至少 2 帧。
template<typename T>
size_t find_key_interval(const std::vector<Keyframe<T>>& keys, float time, float& out_t) {
    // upper_bound 找首个 time > 采样点的帧
    auto it = std::upper_bound(keys.begin(), keys.end(), time,
                               [](float t, const Keyframe<T>& k) { return t < k.time; });
    if (it == keys.begin()) {
        out_t = 0.0f;
        return 0;
    }
    if (it == keys.end()) {
        out_t = 0.0f;
        return keys.size() - 2; // 调用方已 clamp，理论不到达；兜底取末段
    }
    const size_t next = static_cast<size_t>(std::distance(keys.begin(), it));
    const size_t prev = next - 1;
    const float dt = keys[next].time - keys[prev].time;
    out_t = (dt > 1e-8f) ? (time - keys[prev].time) / dt : 0.0f;
    return prev;
}

} // namespace

const BoneTrack* AnimationClip::find_track(int32_t bone_index) const {
    if (bone_index < 0) return nullptr;
    for (const auto& track : tracks) {
        if (track.bone_index == bone_index) return &track;
    }
    return nullptr;
}

float AnimationClip::wrap_time(float time, bool loop) const {
    if (loop && duration > 1e-8f) {
        // fmod 处理负时间：先取模再平移到 [0, duration)
        float t = std::fmod(time, duration);
        if (t < 0.0f) t += duration;
        return t;
    }
    if (time < 0.0f) return 0.0f;
    if (time > duration) return duration;
    return time;
}

math::Vector3f AnimationClip::sample_vec3(const std::vector<Keyframe<math::Vector3f>>& keys,
                                          float time, const math::Vector3f& bind_value) {
    if (keys.empty()) return bind_value;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    float t = 0.0f;
    const size_t i = find_key_interval(keys, time, t);
    return keys[i].value.lerp(keys[i + 1].value, t);
}

math::Quaternionf AnimationClip::sample_rotation(const std::vector<Keyframe<math::Quaternionf>>& keys,
                                                 float time, const math::Quaternionf& bind_value) {
    if (keys.empty()) return bind_value;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    float t = 0.0f;
    const size_t i = find_key_interval(keys, time, t);
    // slerp 内部已处理最短路径（dot<0 翻转）
    return math::Quaternionf::slerp(keys[i].value, keys[i + 1].value, t);
}

math::Matrix4f AnimationClip::sample_bone(int32_t bone_index, float time, bool loop,
                                          const math::Vector3f& bind_position,
                                          const math::Quaternionf& bind_rotation,
                                          const math::Vector3f& bind_scale) const {
    const BoneTrack* track = find_track(bone_index);
    const float t = wrap_time(time, loop);

    math::Vector3f position = bind_position;
    math::Quaternionf rotation = bind_rotation;
    math::Vector3f scale = bind_scale;

    if (track) {
        position = sample_vec3(track->position_keys, t, bind_position);
        rotation = sample_rotation(track->rotation_keys, t, bind_rotation);
        scale = sample_vec3(track->scale_keys, t, bind_scale);
    }

    return math::Matrix4f::translate(position) *
           math::Matrix4f::from_quaternion(rotation) *
           math::Matrix4f::scale(scale);
}

} // namespace gryce_engine::animation
