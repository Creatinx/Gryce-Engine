#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "math/math.h"

// 前向声明 miniaudio 类型，避免在每个包含处引入完整头文件
struct ma_engine;
struct ma_sound;

namespace gryce_engine::audio {

// ---------------------------------------------------------------------------
// AudioEngine — 全局音频引擎（封装 miniaudio）。
// 负责设备初始化、监听器位置、以及 Sound 实例的生命周期。
// ---------------------------------------------------------------------------
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    void shutdown();
    bool valid() const;

    // 监听器位置（用于 3D 空间音频）
    void set_listener_position(const math::Vector3f& pos);
    math::Vector3f listener_position() const;

    // 内部句柄，供 AudioClip / AudioSource 使用
    ma_engine* engine() const { return engine_.get(); }

    static AudioEngine& instance();

private:
    struct EngineDeleter {
        void operator()(ma_engine* e) const;
    };
    std::unique_ptr<ma_engine, EngineDeleter> engine_;
};

// ---------------------------------------------------------------------------
// AudioClip — 已加载的音频片段（可复用）。
// ---------------------------------------------------------------------------
class AudioClip {
public:
    AudioClip();
    ~AudioClip();

    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;

    // 从文件加载（支持 wav/mp3/ogg/flac 等 miniaudio 支持的格式）
    bool load(const std::string& path);
    bool valid() const { return is_streaming_ ? !path_.empty() : sound_ != nullptr; }
    const std::string& path() const { return path_; }
    bool is_streaming() const { return is_streaming_; }

    // 将非流式音频解码成交错 float PCM（sample_rate/channels 一并返回）。
    // 用于变速不变调：先 decode 再时域拉伸，最后从内存播放。
    bool decode_to_pcm(std::vector<float>& out_pcm, uint32_t& out_sample_rate,
                       uint32_t& out_channels) const;

    ma_sound* handle() const { return sound_.get(); }

private:
    struct SoundDeleter {
        void operator()(ma_sound* s) const;
    };
    std::unique_ptr<ma_sound, SoundDeleter> sound_;
    std::string path_;
    bool is_streaming_ = false;
};

// ---------------------------------------------------------------------------
// AudioInstance — 一次性的播放实例。
// 允许同一个 AudioClip 同时播放多次。
// ---------------------------------------------------------------------------
class AudioInstance {
public:
    AudioInstance();
    ~AudioInstance();

    AudioInstance(const AudioInstance&) = delete;
    AudioInstance& operator=(const AudioInstance&) = delete;

    // 从已加载的 clip 创建播放实例
    bool create_from_clip(std::shared_ptr<const AudioClip> clip, float speed = 1.0f);
    bool valid() const { return sound_ != nullptr; }

    void play();
    void stop();
    void set_volume(float volume);
    void set_pitch(float pitch);
    // 变速不变调：底层对解码后 PCM 做时域拉伸；流式音频回退到普通 pitch
    void set_speed(float speed);
    void set_loop(bool loop);
    void set_3d(bool enabled);
    void set_position(const math::Vector3f& pos);
    void set_spatial_range(float min_dist, float max_dist);
    bool is_playing() const;

private:
    struct SoundDeleter {
        void operator()(ma_sound* s) const;
    };
    struct AudioBufferDeleter {
        void operator()(void* b) const;
    };

    bool recreate_from_source(float speed);

    // 注意：sound_ 引用 audio_buffer_/stretched_pcm_ 作为 data source，
    // 因此成员声明顺序必须保证 sound_ 最先被析构。
    std::shared_ptr<const AudioClip> source_clip_;
    std::vector<float> stretched_pcm_;
    // audio_buffer_ 实际指向 ma_audio_buffer（定义在 miniaudio.h），
    // 用 void* 避免在头文件中暴露庞大的 miniaudio.h。
    std::unique_ptr<void, AudioBufferDeleter> audio_buffer_;
    std::unique_ptr<ma_sound, SoundDeleter> sound_;

    float speed_ = 1.0f;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
};

} // namespace gryce_engine::audio
