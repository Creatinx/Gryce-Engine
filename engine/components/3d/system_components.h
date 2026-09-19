#pragma once

#include "components/component.h"
#include "math/math.h"
#include "scene/entity.h"

#include <algorithm>
#include <cmath>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// RayCast3D — 3D 射线检测组件
// 每帧由 PhysicsSystem3D 填充 hit 结果（direction 为本地空间，随实体旋转）。
// ---------------------------------------------------------------------------
class RayCast3D : public Component {
public:
    math::Vector3f direction = math::Vector3f(0.0f, 0.0f, -1.0f);
    float max_distance = 10.0f;
    bool ignore_trigger = true;

    // 运行时结果（只读）
    bool hit = false;
    math::Vector3f hit_point = math::Vector3f::zero();
    math::Vector3f hit_normal = math::Vector3f::zero();
    float hit_distance = 0.0f;

    RayCast3D() = default;
    const char* type() const override { return "RayCast3D"; }

    void serialize(nlohmann::json& out) const override {
        out["direction"] = { direction.x, direction.y, direction.z };
        out["max_distance"] = max_distance;
        out["ignore_trigger"] = ignore_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        auto d = in.value("direction", std::vector<float>{0, 0, -1});
        if (d.size() >= 3) direction = math::Vector3f(d[0], d[1], d[2]);
        max_distance = in.value("max_distance", 10.0f);
        ignore_trigger = in.value("ignore_trigger", true);
    }
};

// ---------------------------------------------------------------------------
// SpringArm3D — 弹簧臂（第三人称相机碰撞）
// 当前实现做平滑跟随；碰撞缩短（RayCast 回缩）由后续系统接入。
// ---------------------------------------------------------------------------
class SpringArm3D : public Component {
public:
    float spring_length = 3.0f;
    float collision_margin = 0.05f;
    float lerp_speed = 10.0f;
    bool collide_with_world = true;

    // 运行时（不序列化）
    math::Vector3f current_offset = math::Vector3f(0.0f, 0.0f, -3.0f);

    SpringArm3D() = default;
    const char* type() const override { return "SpringArm3D"; }

    void on_update(float dt) override {
        const math::Vector3f target = math::Vector3f(0.0f, 0.0f, -std::max(0.0f, spring_length));
        const float alpha = 1.0f - std::exp(-lerp_speed * dt);
        current_offset = current_offset.lerp(target, std::clamp(alpha, 0.0f, 1.0f));
    }

    void serialize(nlohmann::json& out) const override {
        out["spring_length"] = spring_length;
        out["collision_margin"] = collision_margin;
        out["lerp_speed"] = lerp_speed;
        out["collide_with_world"] = collide_with_world;
    }
    void deserialize(const nlohmann::json& in) override {
        spring_length = in.value("spring_length", 3.0f);
        collision_margin = in.value("collision_margin", 0.05f);
        lerp_speed = in.value("lerp_speed", 10.0f);
        collide_with_world = in.value("collide_with_world", true);
        current_offset = math::Vector3f(0.0f, 0.0f, -spring_length);
    }
};

// ---------------------------------------------------------------------------
// VisibilityNotifier3D — 屏幕可见通知（对象池/剔除；渲染系统填充 is_visible）
// ---------------------------------------------------------------------------
class VisibilityNotifier3D : public Component {
public:
    math::Vector3f size = math::Vector3f::one();
    bool is_visible = false;

    VisibilityNotifier3D() = default;
    const char* type() const override { return "VisibilityNotifier3D"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y, size.z };
    }
    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{1, 1, 1});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
    }
};

// ---------------------------------------------------------------------------
// AudioReverbZone — 3D 混响区域（配合 miniaudio 管线）
// ---------------------------------------------------------------------------
class AudioReverbZone : public Component {
public:
    float min_distance = 1.0f;
    float max_distance = 20.0f;
    float reverb_level = 1.0f;
    float decay_time = 1.5f;
    float density = 1.0f;

    AudioReverbZone() = default;
    const char* type() const override { return "AudioReverbZone"; }

    void serialize(nlohmann::json& out) const override {
        out["min_distance"] = min_distance;
        out["max_distance"] = max_distance;
        out["reverb_level"] = reverb_level;
        out["decay_time"] = decay_time;
        out["density"] = density;
    }
    void deserialize(const nlohmann::json& in) override {
        min_distance = in.value("min_distance", 1.0f);
        max_distance = in.value("max_distance", 20.0f);
        reverb_level = in.value("reverb_level", 1.0f);
        decay_time = in.value("decay_time", 1.5f);
        density = in.value("density", 1.0f);
    }
};

} // namespace gryce_engine::components
