#pragma once

#include "components/component.h"
#include "math/math.h"
#include "scene/entity.h"

#include <algorithm>
#include <cmath>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Timer — 通用计时器组件（2D/3D 实体均可挂载）
// ---------------------------------------------------------------------------
class Timer : public Component {
public:
    float wait_time = 1.0f;
    bool one_shot = false;
    bool auto_start = true;

    // 运行时（不序列化）
    float time_left = 0.0f;
    bool is_running = false;
    bool is_finished = false;
    int timeout_count = 0;

    Timer() = default;
    const char* type() const override { return "Timer"; }

    void start() {
        time_left = std::max(0.0f, wait_time);
        is_running = true;
        is_finished = false;
    }
    void stop() {
        is_running = false;
    }
    void reset() {
        time_left = std::max(0.0f, wait_time);
        is_finished = false;
        is_running = auto_start;
    }

    void on_awake() override {
        if (auto_start) start();
    }

    void on_update(float dt) override {
        if (!is_running) return;
        time_left -= dt;
        if (time_left > 0.0f) return;
        ++timeout_count;
        is_finished = true;
        if (one_shot) {
            is_running = false;
            time_left = 0.0f;
        } else {
            time_left += std::max(0.0f, wait_time);
        }
    }

    void serialize(nlohmann::json& out) const override {
        out["wait_time"] = wait_time;
        out["one_shot"] = one_shot;
        out["auto_start"] = auto_start;
    }
    void deserialize(const nlohmann::json& in) override {
        wait_time = in.value("wait_time", 1.0f);
        one_shot = in.value("one_shot", false);
        auto_start = in.value("auto_start", true);
    }
};

// ---------------------------------------------------------------------------
// TweenPlayer — 简单缓动动画组件（Transform 位置/缩放）
// ---------------------------------------------------------------------------
class TweenPlayer : public Component {
public:
    math::Vector3f from = math::Vector3f::zero();
    math::Vector3f to = math::Vector3f::one();
    float duration = 1.0f;
    bool playing = false;
    bool loop = false;
    bool tween_scale = false;
    int easing = 0; // 0=linear, 1=smoothstep

    // 运行时（不序列化）
    float elapsed = 0.0f;

    TweenPlayer() = default;
    const char* type() const override { return "TweenPlayer"; }

    void start() {
        elapsed = 0.0f;
        playing = true;
    }

    void on_update(float dt) override {
        if (!playing || !owner_ || !owner_->transform()) return;
        elapsed += dt;
        float t = duration > 1e-5f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
        if (easing == 1) t = t * t * (3.0f - 2.0f * t); // smoothstep
        math::Vector3f value = from.lerp(to, t);
        auto* tr = owner_->transform();
        if (tween_scale) {
            tr->scale = value;
        } else {
            tr->position = value;
        }
        if (t >= 1.0f) {
            if (loop) {
                elapsed = 0.0f;
            } else {
                playing = false;
            }
        }
    }

    void serialize(nlohmann::json& out) const override {
        out["from"] = { from.x, from.y, from.z };
        out["to"] = { to.x, to.y, to.z };
        out["duration"] = duration;
        out["playing"] = playing;
        out["loop"] = loop;
        out["tween_scale"] = tween_scale;
        out["easing"] = easing;
    }
    void deserialize(const nlohmann::json& in) override {
        auto f = in.value("from", std::vector<float>{0, 0, 0});
        if (f.size() >= 3) from = math::Vector3f(f[0], f[1], f[2]);
        auto t = in.value("to", std::vector<float>{1, 1, 1});
        if (t.size() >= 3) to = math::Vector3f(t[0], t[1], t[2]);
        duration = in.value("duration", 1.0f);
        playing = in.value("playing", false);
        loop = in.value("loop", false);
        tween_scale = in.value("tween_scale", false);
        easing = in.value("easing", 0);
    }
};

} // namespace gryce_engine::components
