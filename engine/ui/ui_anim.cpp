// ===========================================================================
// engine/ui/ui_anim.cpp — UI 动画系统的实现（纯函数层 + 运行时）
//
// 无 ImGui 依赖：动画是数据与数学，与怎么画无关。整份文件在关掉 ImGui 的
// 构建里照常编译（无窗口宿主也能跑动画逻辑，单测因此不需要窗口）。
// ===========================================================================

#include "ui/ui_anim.h"

#include <cmath>
#include <cstring>

namespace gryce_ui {

namespace {

const char* const kEaseNames[kAnimEaseCount] = {
    "Linear",
    "EaseInQuad",
    "EaseOutQuad",
    "EaseInOutQuad",
    "EaseInCubic",
    "EaseOutCubic",
    "EaseInOutCubic",
    "EaseOutBack",
    "EaseInOutBack",
    "EaseOutElastic",
    "EaseOutBounce",
};

// 定长字符名的拷贝/比较。超长静默截断（通道名是给人看的标识，不值得为它
// 引入动态分配；截断了也能从上下文认出来）。
void copy_name(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src) {
        for (; i < cap - 1 && src[i] != '\0'; ++i) dst[i] = src[i];
    }
    dst[i] = '\0';
}

bool same_name(const char* a, const char* b) {
    return std::strncmp(a, b, kAnimChannelNameMax) == 0;
}

constexpr float kPi = 3.14159265f;

} // namespace

// ---------------------------------------------------------------------------
// 块一：纯函数层
// ---------------------------------------------------------------------------

const char* anim_ease_name(AnimEase e) {
    const int i = static_cast<int>(e);
    if (i < 0 || i >= kAnimEaseCount) return "";
    return kEaseNames[i];
}

float anim_ease(AnimEase e, float t) {
    // 边界对齐：所有缓动 f(0)=0、f(1)=1；NaN 也会被 !(t>0) 吃掉变成 0。
    if (!(t > 0.0f)) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (e) {
        case AnimEase::Linear:       return t;
        case AnimEase::EaseInQuad:   return t * t;
        case AnimEase::EaseOutQuad:  return t * (2.0f - t);
        case AnimEase::EaseInOutQuad:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        case AnimEase::EaseInCubic:  return t * t * t;
        case AnimEase::EaseOutCubic: {
            const float u = t - 1.0f;
            return u * u * u + 1.0f;
        }
        case AnimEase::EaseInOutCubic:
            return t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case AnimEase::EaseOutBack: {
            const float c1 = 1.70158f;
            const float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case AnimEase::EaseInOutBack: {
            const float c1 = 1.70158f;
            const float c2 = c1 * 1.525f;
            if (t < 0.5f) {
                return (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f;
            }
            return (std::pow(2.0f * t - 2.0f, 2.0f) *
                    ((c2 + 1.0f) * (2.0f * t - 2.0f) + c2) + 2.0f) / 2.0f;
        }
        case AnimEase::EaseOutElastic: {
            const float c4 = (2.0f * kPi) / 3.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
        case AnimEase::EaseOutBounce: {
            const float n1 = 7.5625f;
            const float d1 = 2.75f;
            if (t < 1.0f / d1) return n1 * t * t;
            if (t < 2.0f / d1) {
                const float u = t - 1.5f / d1;
                return n1 * u * u + 0.75f;
            }
            if (t < 2.5f / d1) {
                const float u = t - 2.25f / d1;
                return n1 * u * u + 0.9375f;
            }
            const float u = t - 2.625f / d1;
            return n1 * u * u + 0.984375f;
        }
        default: return t;
    }
}

float AnimTween::value_at(float t) const {
    if (duration <= 0.0f) return to;
    if (t <= delay) return from;
    const float local = (t - delay) / duration;
    if (local >= 1.0f) return to;
    return from + (to - from) * anim_ease(ease, local);
}

AnimColor anim_color_lerp(const AnimColor& a, const AnimColor& b, float t) {
    if (!(t > 0.0f)) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float ia = 1.0f - t;
    AnimColor out;
    out.r = a.r * ia + b.r * t;
    out.g = a.g * ia + b.g * t;
    out.b = a.b * ia + b.b * t;
    out.a = a.a * ia + b.a * t;
    return out;
}

bool AnimTrack::add(float t, float v, AnimEase ease) {
    int pos = 0;
    while (pos < count_ && keys_[pos].t < t) ++pos;
    if (pos < count_ && keys_[pos].t == t) {
        // 同一时刻：覆盖值与缓动（时间轴上同一帧不存两个值）
        keys_[pos].v = v;
        keys_[pos].ease = ease;
        return true;
    }
    if (count_ >= kMaxKeys) return false;
    for (int i = count_; i > pos; --i) keys_[i] = keys_[i - 1];
    keys_[pos].t = t;
    keys_[pos].v = v;
    keys_[pos].ease = ease;
    ++count_;
    return true;
}

void AnimTrack::clear() { count_ = 0; }

int AnimTrack::count() const { return count_; }

const AnimKey& AnimTrack::key(int index) const {
    static const AnimKey kEmpty{};
    if (index < 0 || index >= count_) return kEmpty;
    return keys_[index];
}

float AnimTrack::eval(float t) const {
    if (count_ == 0) return 0.0f;
    if (count_ == 1 || t <= keys_[0].t) return keys_[0].v;
    const AnimKey& last = keys_[count_ - 1];
    if (t >= last.t) return last.v;
    for (int i = 0; i < count_ - 1; ++i) {
        if (t < keys_[i + 1].t) {
            // add() 保证不存在重复 t，所以 t1 > t0，除数不会为 0
            const float t0 = keys_[i].t;
            const float t1 = keys_[i + 1].t;
            const float local = (t - t0) / (t1 - t0);
            const float e = anim_ease(keys_[i].ease, local);
            return keys_[i].v + (keys_[i + 1].v - keys_[i].v) * e;
        }
    }
    return last.v;
}

float AnimTrack::duration() const {
    return count_ > 0 ? keys_[count_ - 1].t : 0.0f;
}

// ---------------------------------------------------------------------------
// 块二：运行时
// ---------------------------------------------------------------------------

bool AnimClip::add_track(const AnimClipTrack& track) {
    for (int i = 0; i < track_count_; ++i) {
        if (same_name(tracks_[i].channel, track.channel)) {
            tracks_[i] = track;   // 同名通道整体覆盖，保持原位置
            return true;
        }
    }
    if (track_count_ >= kMaxTracks) return false;
    tracks_[track_count_++] = track;
    return true;
}

void AnimClip::clear() { track_count_ = 0; }

int AnimClip::track_count() const { return track_count_; }

const AnimClipTrack& AnimClip::track(int index) const {
    static const AnimClipTrack kEmpty{};
    if (index < 0 || index >= track_count_) return kEmpty;
    return tracks_[index];
}

float AnimClip::computed_duration() const {
    if (duration > 0.0f) return duration;
    float d = 0.0f;
    for (int i = 0; i < track_count_; ++i) {
        const AnimClipTrack& tr = tracks_[i];
        const float len = tr.is_color ? tr.duration : tr.keys.duration();
        const float end = tr.delay + (len > 0.0f ? len : 0.0f);
        if (end > d) d = end;
    }
    return d;
}

void Animator::play(const AnimClip& clip, bool restart) {
    clip_ = clip;   // 拷贝：调用方的片段之后可以随便改，不影响正在播的
    has_clip_ = true;
    if (restart) {
        time_ = 0.0f;
        finished_ = false;
    }
    // 通道表以本片段的轨道为准重建：不重建的话换片段后会读到上一段的残留值
    channel_count_ = 0;
    for (int i = 0; i < clip_.track_count(); ++i) {
        const AnimClipTrack& tr = clip_.track(i);
        Channel& ch = ensure_channel(tr.channel, tr.is_color != 0);
        if (tr.is_color) {
            ch.c = tr.color_from;
        } else if (tr.keys.count() > 0) {
            ch.f = tr.keys.key(0).v;   // 初始值 = 首帧值（delay 期间也是它）
        }
    }
    playing_ = true;
    evaluate();
}

void Animator::stop() {
    playing_ = false;
    finished_ = false;
}

void Animator::pause() { playing_ = false; }

void Animator::resume() {
    if (!has_clip_) return;
    if (finished_) {
        // 播完之后 resume：从头再来，而不是"立刻又瞬间播完"
        time_ = 0.0f;
        finished_ = false;
    }
    playing_ = true;
}

void Animator::set_speed(float speed) {
    if (!(speed > 0.0f)) speed = 0.0f;
    if (speed > 64.0f) speed = 64.0f;
    speed_ = speed;
}

void Animator::seek(float t) {
    if (!(t > 0.0f)) t = 0.0f;
    time_ = t;
    finished_ = false;
    evaluate();
}

float Animator::local_time() const {
    const float dur = clip_.computed_duration();
    if (dur <= 0.0f) return 0.0f;
    switch (clip_.loop) {
        case AnimLoop::Loop: {
            float t = std::fmod(time_, dur);
            if (t < 0.0f) t += dur;
            return t;
        }
        case AnimLoop::PingPong: {
            const float period = dur * 2.0f;
            float phase = std::fmod(time_, period);
            if (phase < 0.0f) phase += period;
            return phase <= dur ? phase : period - phase;
        }
        default:
            return time_ < dur ? time_ : dur;
    }
}

void Animator::advance(float dt) {
    if (!playing_ || !has_clip_) return;
    const float dur = clip_.computed_duration();
    if (dur <= 0.0f) {
        evaluate();
        return;
    }
    time_ += dt * speed_;
    if (time_ < 0.0f) time_ = 0.0f;
    if (clip_.loop == AnimLoop::Once && time_ >= dur) {
        time_ = dur;
        playing_ = false;
        finished_ = true;
    }
    evaluate();
}

void Animator::evaluate() {
    if (!has_clip_) return;
    const float lt = local_time();
    for (int i = 0; i < clip_.track_count(); ++i) {
        const AnimClipTrack& tr = clip_.track(i);
        Channel* ch = find_channel(tr.channel);
        if (!ch) continue;
        float local = lt - tr.delay;
        if (local < 0.0f) local = 0.0f;   // 延迟期间 = 初始值（标量首帧/颜色起点）
        if (tr.is_color) {
            const float k = tr.duration > 0.0f ? local / tr.duration : 1.0f;
            ch->c = anim_color_lerp(tr.color_from, tr.color_to, anim_ease(tr.ease, k));
        } else {
            ch->f = tr.keys.eval(local);
        }
    }
}

Animator::Channel* Animator::find_channel(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < channel_count_; ++i) {
        if (same_name(channels_[i].name, name)) return &channels_[i];
    }
    return nullptr;
}

Animator::Channel& Animator::ensure_channel(const char* name, bool is_color) {
    if (Channel* existing = find_channel(name)) {
        existing->is_color = is_color;
        return *existing;
    }
    // 片段的轨道数不超过 kMaxTracks(8) 且每次 play 重建通道表，正常情况下
    // 永远不会走到满表分支；兜底用最后一个槽而不是写越界。
    const int slot = channel_count_ < kMaxChannels ? channel_count_++ : kMaxChannels - 1;
    Channel& ch = channels_[slot];
    copy_name(ch.name, kAnimChannelNameMax, name ? name : "");
    ch.f = 0.0f;
    ch.c = AnimColor{};
    ch.is_color = is_color;
    return ch;
}

bool Animator::is_playing() const { return playing_; }

bool Animator::is_finished() const { return finished_; }

float Animator::time() const { return time_; }

float Animator::speed() const { return speed_; }

int Animator::channel_count() const { return channel_count_; }

const char* Animator::channel_name(int index) const {
    if (index < 0 || index >= channel_count_) return "";
    return channels_[index].name;
}

bool Animator::channel_float(const char* name, float* out) const {
    if (!name) return false;
    for (int i = 0; i < channel_count_; ++i) {
        if (same_name(channels_[i].name, name)) {
            if (channels_[i].is_color) return false;
            if (out) *out = channels_[i].f;
            return true;
        }
    }
    return false;
}

bool Animator::channel_color(const char* name, AnimColor* out) const {
    if (!name || !out) return false;
    for (int i = 0; i < channel_count_; ++i) {
        if (same_name(channels_[i].name, name)) {
            if (!channels_[i].is_color) return false;
            *out = channels_[i].c;
            return true;
        }
    }
    return false;
}

} // namespace gryce_ui
