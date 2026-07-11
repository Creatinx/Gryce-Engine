#pragma once

#include <memory>
#include <string>

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
    bool valid() const { return sound_ != nullptr; }
    const std::string& path() const { return path_; }

    ma_sound* handle() const { return sound_.get(); }

private:
    struct SoundDeleter {
        void operator()(ma_sound* s) const;
    };
    std::unique_ptr<ma_sound, SoundDeleter> sound_;
    std::string path_;
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
    bool create_from_clip(const AudioClip& clip);
    bool valid() const { return sound_ != nullptr; }

    void play();
    void stop();
    void set_volume(float volume);
    void set_pitch(float pitch);
    void set_loop(bool loop);
    void set_3d(bool enabled);
    void set_position(const math::Vector3f& pos);
    void set_spatial_range(float min_dist, float max_dist);
    bool is_playing() const;

private:
    struct SoundDeleter {
        void operator()(ma_sound* s) const;
    };
    std::unique_ptr<ma_sound, SoundDeleter> sound_;
};

} // namespace gryce_engine::audio
