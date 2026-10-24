// ===========================================================================
// engine/ui/ui_anim.h — UI 动画系统（两个块：纯函数层 + 运行时）
//
// 为什么分成两块：
//   * 块一（anim_ease / AnimTween / anim_color_lerp / AnimTrack）是**纯函数**：
//     给定时间就算出值，不持有任何跨帧状态。绝大多数"属性更改"场景只需要
//     它——按钮hover抬起、面板滑入、数值滚动，都是"某一帧知道 elapsed 就能
//     算出一个属性值"。调用方直接拿返回值写属性，不需要引入第二个概念。
//   * 块二（AnimClip / Animator）是**运行时**：多轨道、具名通道、循环模式、
//     播放控制。当一个界面同时跑好几段交互动画（错峰入场、颜色循环、脉冲），
//     每段都自己记 time/delay 就会散得到处都是，这时才需要它。
// 两块的边界：运行时内部全部由块一的纯函数实现（evaluate 就是反复调用
// anim_ease / AnimTrack::eval），不重复造第二套数学。
//
// 与组件树的关系：动画系统不认识组件。Animator 只往"具名通道"里写值，
// 由宿主决定把通道值用到哪里（组件 rect、窗口位置、任何数值属性）。
// 这样动画不依赖 UI 结构，UI 结构也不依赖动画。
//
// 线程模型：与 ui_theme 相同——纯数据、UI 线程，无锁。advance() 的时间源
// 由调用方给（帧 dt），模型层不自己读时钟，因此单测里时间是精确的。
// ===========================================================================

#ifndef GRYCE_UI_ANIM_H
#define GRYCE_UI_ANIM_H

#include <cstdint>

namespace gryce_ui {

// ---------------------------------------------------------------------------
// 块一：纯函数层
// ---------------------------------------------------------------------------

enum class AnimEase : uint32_t {
    Linear = 0,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseOutBack,      // 回弹（会超出 1 再回落）
    EaseInOutBack,
    EaseOutElastic,   // 弹性（会过冲并振荡）
    EaseOutBounce,    // 落地反弹
    Count,
};

constexpr int kAnimEaseCount = static_cast<int>(AnimEase::Count);

// 显示名（越界返回 ""）。
const char* anim_ease_name(AnimEase e);

// 缓动函数。t 任意实数，内部先夹到 [0,1]；返回值对"边界对齐"的缓动恒有
// f(0)=0、f(1)=1，Back/Elastic 在区间中段会超出 [0,1]（这正是回弹感）。
float anim_ease(AnimEase e, float t);

// 一个标量属性的补间：t 是从"补间开始"起算的局部时间（秒）。
//   t < delay            -> from（延迟期间不动）
//   t >= delay+duration  -> to（精确落到终点，不受缓动形状影响）
//   duration <= 0        -> to（零时长补间就是瞬间到位）
struct AnimTween {
    float   from     = 0.0f;
    float   to       = 0.0f;
    float   duration = 0.25f;
    float   delay    = 0.0f;
    AnimEase ease    = AnimEase::EaseInOutQuad;

    float value_at(float t) const;
};

// RGBA 颜色（0~1 浮点，与 GUi_ColorEdit4 / ImGui 的缓冲同构）。
struct AnimColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

AnimColor anim_color_lerp(const AnimColor& a, const AnimColor& b, float t);

// 同一通道上的有序关键帧。段内缓动由**前一个关键帧**的 ease 决定
// （"从这一帧到下一帧用什么曲线"），与常见的时间轴编辑器一致。
struct AnimKey {
    float   t    = 0.0f;
    float   v    = 0.0f;
    AnimEase ease = AnimEase::Linear;
};

class AnimTrack {
public:
    static constexpr int kMaxKeys = 16;

    // 按 t 有序插入；t 与已有关键帧相同则覆盖其值与缓动。满返回 false。
    bool add(float t, float v, AnimEase ease);
    void clear();

    int              count() const;
    const AnimKey&   key(int index) const;

    // 求值：首帧之前保持首帧值，尾帧之后保持尾帧值（不插到区间外）。
    float eval(float t) const;

    // 尾帧时间（0 = 空轨道）。
    float duration() const;

private:
    int     count_ = 0;
    AnimKey keys_[kMaxKeys];
};

// ---------------------------------------------------------------------------
// 块二：运行时
// ---------------------------------------------------------------------------

enum class AnimLoop : uint32_t {
    Once = 0,      // 播到结尾停下（finished）
    Loop = 1,      // 到头回到开头
    PingPong = 2,  // 到头反向（0->1->0）
};

constexpr int kAnimChannelNameMax = 24;   // 通道名上限（含 '\0'）
constexpr int kAnimClipNameMax    = 32;

// 片段里的一条轨道：把值写进具名通道。
//   标量轨道：is_color=0，用 keys（关键帧，各自带段缓动）。
//   颜色轨道：is_color=1，用 color_from/color_to + duration + ease（端点插值）。
// delay 是轨道级延迟（错峰入场）：局部时间 = 片段时间 - delay，为负时保持
// 初始值（标量 = 首帧值，颜色 = color_from）。
struct AnimClipTrack {
    char     channel[kAnimChannelNameMax] = {};
    float    delay    = 0.0f;
    int      is_color = 0;
    AnimEase ease     = AnimEase::EaseInOutQuad;
    float    duration = 0.25f;                 // 颜色轨道时长
    AnimColor color_from{};
    AnimColor color_to{1.0f, 1.0f, 1.0f, 1.0f};
    AnimTrack keys;                            // 标量轨道
};

class AnimClip {
public:
    static constexpr int kMaxTracks = 8;

    char     name[kAnimClipNameMax] = {};
    float    duration = 0.0f;      // >0 = 显式时长；0 = 由轨道推出
    AnimLoop loop     = AnimLoop::Once;

    // 追加轨道；channel 与已有轨道同名则整体覆盖（保持原位置）。满返回 false。
    bool add_track(const AnimClipTrack& track);
    void clear();

    int                   track_count() const;
    const AnimClipTrack&  track(int index) const;

    // 实际时长：显式 duration 优先，否则取所有轨道的 (delay + 时长) 最大值。
    float computed_duration() const;

private:
    int           track_count_ = 0;
    AnimClipTrack tracks_[kMaxTracks];
};

// 播放器：进程内通常一个就够（演示里同时只有一段交互动画），但这是普通对象，
// 想要多个（比如界面动画与场景动画各一个）就直接实例化两个。
class Animator {
public:
    static constexpr int kMaxChannels = 32;

    // 开始播放（内部拷贝一份片段，调用方的片段之后可以随便改）。
    // restart=false 时保留当前时间（换循环模式/改参数时用）。
    void play(const AnimClip& clip, bool restart);
    void stop();        // 停止播放；通道保留最后的值（画面定格在当前位置）
    void pause();
    void resume();
    void set_speed(float speed);   // 钳到 [0, 64]
    void seek(float t);            // 跳到指定时间并立即评估

    // 按 dt 秒推进并评估全部轨道。未播放时是 no-op。
    void advance(float dt);

    bool  is_playing() const;
    bool  is_finished() const;     // Once 模式自然播完为真
    float time() const;            // 原始累计时间（未按循环模式折叠）
    float speed() const;

    int         channel_count() const;
    const char* channel_name(int index) const;
    // 读通道。标量通道用 channel_float，颜色通道用 channel_color；
    // 没有该通道或类型不匹配时返回 false（不创建通道）。
    bool  channel_float(const char* name, float* out) const;
    bool  channel_color(const char* name, AnimColor* out) const;

private:
    struct Channel {
        char      name[kAnimChannelNameMax] = {};
        float     f     = 0.0f;
        AnimColor c{};
        bool      is_color = false;
    };

    AnimClip  clip_{};
    bool      has_clip_    = false;
    float     time_        = 0.0f;
    float     speed_       = 1.0f;
    bool      playing_     = false;
    bool      finished_    = false;
    int       channel_count_ = 0;
    Channel   channels_[kMaxChannels];

    float    local_time() const;               // 按循环模式折叠后的评估时间
    void     evaluate();
    Channel* find_channel(const char* name);
    Channel& ensure_channel(const char* name, bool is_color);
};

} // namespace gryce_ui

#endif // GRYCE_UI_ANIM_H
