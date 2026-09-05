#pragma once

// GryceEngineUtils::ui::animation.h — 动画系统
//
// 属性动画：淡入淡出 / 滑入滑出 / 缩放 / 颜色过渡。
// 支持链式（then）与并行（parallel），缓动函数，循环与反向播放。

#include <string>

#include "GryceEngineUtils/ui/signal.h"
#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

enum class Easing {
    Linear, EaseIn, EaseOut, EaseInOut
};

enum class AnimationType {
    Fade, Slide, Scale, Color
};

enum class Direction {
    FromTop, FromBottom, FromLeft, FromRight
};

class UIManager;

class Animation {
public:
    // 创建动画（自动注册到当前 UIManager）
    static Animation* create(Widget* target, float duration,
                             AnimationType type, Easing easing = Easing::Linear);

    static Animation* fade_in(Widget* target, float duration);
    static Animation* fade_out(Widget* target, float duration);
    static Animation* slide_in(Widget* target, float duration, Direction dir);
    static Animation* slide_out(Widget* target, float duration, Direction dir);
    static Animation* scale(Widget* target, float duration,
                            float from, float to);
    static Animation* color_transition(Widget* target, float duration,
                                       const Color& from, const Color& to,
                                       const char* property = "background");

    // 控制
    void play();
    void pause();
    void stop();
    void set_loop(bool loop) { loop_ = loop; }
    void set_reverse(bool reverse) { reverse_ = reverse; }
    bool is_playing() const { return playing_; }

    // 链式 / 并行
    Animation* then(Animation* next);
    Animation* parallel(Animation* together);

    Signal<void()> on_finished;

    // 框架内部调用（UIManager::update 驱动）
    void update(float dt);

    float duration() const { return duration_; }
    bool finished() const { return finished_; }
    bool valid() const { return target_ != nullptr && *target_alive_; }

private:
    Animation(Widget* target, float duration, AnimationType type, Easing easing);

    void apply(float t);
    static float ease(Easing easing, float t);
    static float bounce_out(float t);

    Widget* target_ = nullptr;
    std::shared_ptr<bool> target_alive_;
    float duration_ = 0.0f;
    float elapsed_ = 0.0f;
    AnimationType type_ = AnimationType::Fade;
    Easing easing_ = Easing::Linear;
    Direction direction_ = Direction::FromBottom;

    bool playing_ = false;
    bool finished_ = false;
    bool loop_ = false;
    bool reverse_ = false;
    bool apply_reverse_ = false;

    // 起始值（play 时捕获）
    float base_opacity_ = 1.0f;
    float base_x_ = 0.0f;
    float base_y_ = 0.0f;
    float base_scale_ = 1.0f;
    Color base_color_;
    float from_ = 0.0f;
    float to_ = 1.0f;
    Color color_from_;
    Color color_to_;

    Animation* next_ = nullptr;
    Animation* together_ = nullptr;

    friend class UIManager;
};

} // namespace GryceEngineUtils::ui
