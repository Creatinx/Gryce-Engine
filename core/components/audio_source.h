#pragma once

#include <memory>
#include <string>
#include <vector>

#include "components/component.h"
#include "audio/audio_engine.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// AudioSource — 音频源组件。
// 负责在 Entity 位置播放音频片段，支持 2D/3D 空间化、音量、音高、循环。
// ---------------------------------------------------------------------------
class AudioSource : public Component {
public:
    std::string clip_path;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool play_on_awake = false;
    bool is_3d = true;
    float min_distance = 1.0f;
    float max_distance = 100.0f;

    AudioSource() = default;

    const char* type() const override { return "AudioSource"; }

    void serialize(nlohmann::json& out) const override {
        out["clip_path"] = clip_path;
        out["volume"] = volume;
        out["pitch"] = pitch;
        out["loop"] = loop;
        out["play_on_awake"] = play_on_awake;
        out["is_3d"] = is_3d;
        out["min_distance"] = min_distance;
        out["max_distance"] = max_distance;
    }

    void deserialize(const nlohmann::json& in) override {
        clip_path = in.value("clip_path", "");
        volume = in.value("volume", 1.0f);
        pitch = in.value("pitch", 1.0f);
        loop = in.value("loop", false);
        play_on_awake = in.value("play_on_awake", false);
        is_3d = in.value("is_3d", true);
        min_distance = in.value("min_distance", 1.0f);
        max_distance = in.value("max_distance", 100.0f);
    }

    void on_awake() override;
    void on_init() override;
    void on_start() override;
    void on_enable() override;
    void on_disable() override;
    void on_update(float dt) override;
    void on_destroy() override;

    // 立即播放一次；若当前已有非循环实例在播放，会先停止。
    void play();
    void stop();
    bool is_playing() const;

private:
    void ensure_clip_loaded();
    void sync_to_owner();

    std::shared_ptr<audio::AudioClip> clip_;
    std::vector<std::unique_ptr<audio::AudioInstance>> instances_;
    bool started_on_awake_ = false;
};

} // namespace gryce_engine::components
