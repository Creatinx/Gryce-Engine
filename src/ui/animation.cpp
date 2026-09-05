#include "GryceEngineUtils/ui/animation.h"

#include <algorithm>
#include <cmath>

#include "GryceEngineUtils/ui/ui.h"

namespace GryceEngineUtils::ui {

Animation* Animation::create(Widget* target, float duration,
                             AnimationType type, Easing easing) {
    auto* a = new Animation(target, duration, type, easing);
    if (UIManager* m = UIManager::current()) {
        m->add_animation(a);
    }
    return a;
}

Animation* Animation::fade_in(Widget* target, float duration) {
    auto* a = create(target, duration, AnimationType::Fade, Easing::EaseOut);
    a->from_ = 0.0f;
    a->to_ = 1.0f;
    return a;
}

Animation* Animation::fade_out(Widget* target, float duration) {
    auto* a = create(target, duration, AnimationType::Fade, Easing::EaseIn);
    a->from_ = 1.0f;
    a->to_ = 0.0f;
    return a;
}

Animation* Animation::slide_in(Widget* target, float duration, Direction dir) {
    auto* a = create(target, duration, AnimationType::Slide, Easing::EaseOut);
    a->direction_ = dir;
    return a;
}

Animation* Animation::slide_out(Widget* target, float duration, Direction dir) {
    auto* a = create(target, duration, AnimationType::Slide, Easing::EaseIn);
    a->direction_ = dir;
    a->apply_reverse_ = true; // 从原位滑出
    return a;
}

Animation* Animation::scale(Widget* target, float duration,
                            float from, float to) {
    auto* a = create(target, duration, AnimationType::Scale, Easing::EaseOut);
    a->from_ = from;
    a->to_ = to;
    return a;
}

Animation* Animation::color_transition(Widget* target, float duration,
                                       const Color& from, const Color& to,
                                       const char* property) {
    (void)property;
    auto* a = create(target, duration, AnimationType::Color, Easing::EaseInOut);
    a->color_from_ = from;
    a->color_to_ = to;
    return a;
}

Animation* Animation::then(Animation* next) {
    next_ = next;
    return next;
}

Animation* Animation::parallel(Animation* together) {
    together_ = together;
    if (together) together->play();
    return this;
}

void Animation::play() {
    if (!target_ || !*target_alive_) return;
    base_opacity_ = target_->opacity();
    base_x_ = target_->position_x();
    base_y_ = target_->position_y();
    base_scale_ = target_->scale();
    base_color_ = target_->style_.background;
    elapsed_ = 0.0f;
    finished_ = false;
    playing_ = true;
}

void Animation::pause() {
    playing_ = false;
}

void Animation::stop() {
    playing_ = false;
    elapsed_ = 0.0f;
    finished_ = true;
}

void Animation::update(float dt) {
    if (!playing_ || finished_) return;
    if (!valid()) {
        finished_ = true;
        return;
    }

    elapsed_ += dt;
    float t = duration_ > 0.0f ? std::clamp(elapsed_ / duration_, 0.0f, 1.0f) : 1.0f;
    const float eased = ease(easing_, t);
    apply(reverse_ ? (1.0f - eased) : eased);

    if (together_) together_->update(dt);

    if (elapsed_ >= duration_) {
        if (loop_) {
            elapsed_ = 0.0f;
            return;
        }
        finished_ = true;
        playing_ = false;
        on_finished.emit();
        if (next_) next_->play();
    }
}

void Animation::apply(float t) {
    switch (type_) {
        case AnimationType::Fade: {
            const float target_opacity = base_opacity_ * (from_ + (to_ - from_) * t);
            target_->set_opacity(std::clamp(target_opacity, 0.0f, 1.0f));
            break;
        }
        case AnimationType::Slide: {
            const float h = target_->bounds().h > 0.0f ? target_->bounds().h : 40.0f;
            const float w = target_->bounds().w > 0.0f ? target_->bounds().w : 120.0f;
            float ox = 0.0f, oy = 0.0f;
            switch (direction_) {
                case Direction::FromTop: oy = -h; break;
                case Direction::FromBottom: oy = h; break;
                case Direction::FromLeft: ox = -w; break;
                case Direction::FromRight: ox = w; break;
            }
            if (apply_reverse_) {
                // 滑出：从原位移动到偏移处
                target_->set_position(base_x_ + ox * t, base_y_ + oy * t);
            } else {
                // 滑入：从偏移处移动到原位
                target_->set_position(base_x_ + ox * (1.0f - t), base_y_ + oy * (1.0f - t));
            }
            break;
        }
        case AnimationType::Scale: {
            target_->set_scale(base_scale_ * (from_ + (to_ - from_) * t));
            break;
        }
        case AnimationType::Color: {
            const Color c(
                color_from_.r + (color_to_.r - color_from_.r) * t,
                color_from_.g + (color_to_.g - color_from_.g) * t,
                color_from_.b + (color_to_.b - color_from_.b) * t,
                color_from_.a + (color_to_.a - color_from_.a) * t);
            target_->style_.background = c;
            target_->style_.has_background = true;
            break;
        }
    }
}

float Animation::ease(Easing easing, float t) {
    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::EaseIn:
            return t * t;
        case Easing::EaseOut:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case Easing::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }
    return t;
}

float Animation::bounce_out(float t) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

Animation::Animation(Widget* target, float duration, AnimationType type, Easing easing)
    : target_(target)
    , target_alive_(target ? target->signal_lifetime() : std::make_shared<bool>(false))
    , duration_(std::max(0.0f, duration))
    , type_(type)
    , easing_(easing) {}

} // namespace GryceEngineUtils::ui
