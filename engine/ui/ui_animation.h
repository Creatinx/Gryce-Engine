#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

#include "math/math.h"

namespace gryce_engine::ui {

enum class UIEasing : uint8_t {
    Linear = 0,
    OutQuad,
    OutCubic,
    InOutCubic,
    OutBack,
};

inline float ui_ease(UIEasing easing, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing) {
        case UIEasing::OutQuad:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case UIEasing::OutCubic:
            return 1.0f - std::pow(1.0f - t, 3.0f);
        case UIEasing::InOutCubic:
            return t < 0.5f
                       ? 4.0f * t * t * t
                       : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        case UIEasing::OutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float p = t - 1.0f;
            return 1.0f + c3 * p * p * p + c1 * p * p;
        }
        case UIEasing::Linear:
        default:
            return t;
    }
}

struct UIFloatTween {
    bool active = false;
    float from = 0.0f;
    float to = 0.0f;
    float value = 0.0f;
    float delay = 0.0f;
    float duration = 0.0f;
    float elapsed = 0.0f;
    UIEasing easing = UIEasing::OutCubic;

    void reset(float current) {
        active = false;
        from = to = value = current;
        delay = duration = elapsed = 0.0f;
    }

    void start(float current, float target, float seconds,
               UIEasing curve = UIEasing::OutCubic, float wait = 0.0f) {
        from = value = current;
        to = target;
        duration = std::max(0.0f, seconds);
        delay = std::max(0.0f, wait);
        elapsed = 0.0f;
        easing = curve;
        active = duration > 0.0f || delay > 0.0f;
        if (!active) value = to;
    }

    bool advance(float dt) {
        if (!active) return false;
        if (delay > 0.0f) {
            delay -= std::max(0.0f, dt);
            if (delay > 0.0f) return false;
        }
        const float previous = value;
        elapsed += std::max(0.0f, dt);
        if (duration <= 0.0f || elapsed >= duration) {
            value = to;
            active = false;
        } else {
            value = from + (to - from) * ui_ease(easing, elapsed / duration);
        }
        return value != previous;
    }
};

struct UIVector2Tween {
    bool active = false;
    math::Vector2f from = math::Vector2f::zero();
    math::Vector2f to = math::Vector2f::zero();
    math::Vector2f value = math::Vector2f::zero();
    float delay = 0.0f;
    float duration = 0.0f;
    float elapsed = 0.0f;
    UIEasing easing = UIEasing::OutCubic;

    void reset(const math::Vector2f& current) {
        active = false;
        from = to = value = current;
        delay = duration = elapsed = 0.0f;
    }

    void start(const math::Vector2f& current, const math::Vector2f& target,
               float seconds, UIEasing curve = UIEasing::OutCubic,
               float wait = 0.0f) {
        from = value = current;
        to = target;
        duration = std::max(0.0f, seconds);
        delay = std::max(0.0f, wait);
        elapsed = 0.0f;
        easing = curve;
        active = duration > 0.0f || delay > 0.0f;
        if (!active) value = to;
    }

    bool advance(float dt) {
        if (!active) return false;
        if (delay > 0.0f) {
            delay -= std::max(0.0f, dt);
            if (delay > 0.0f) return false;
        }
        const math::Vector2f previous = value;
        elapsed += std::max(0.0f, dt);
        if (duration <= 0.0f || elapsed >= duration) {
            value = to;
            active = false;
        } else {
            const float eased = ui_ease(easing, elapsed / duration);
            value = from + (to - from) * eased;
        }
        return value.x != previous.x || value.y != previous.y;
    }
};

} // namespace gryce_engine::ui
