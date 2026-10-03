#pragma once

#include "components/component.h"
#include "math/math.h"

#include <cstdint>
#include <string>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// 音频组件族（底层 Amplitude Audio SDK，由 AudioSystem 驱动）
//
// 坐标约定：引擎世界为右手系、+Y 向上，与 Amplitude 的 hmm_vec3 一致，
// AudioSystem 每帧把 Transform 的位置/朝向同步给监听者与发射器。
//
// 事件播放依赖 Amplitude 的 sound bank（.ambank）中已注册的事件名；
// 未加载任何音效库时，播放调用会安全地空转并记录警告。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AudioListener — 听觉监听者（通常挂在相机实体上）
// 场景中应只有一个启用中的 AudioListener；系统取第一个并设为默认监听者。
// ---------------------------------------------------------------------------
class AudioListener : public Component {
public:
    float gain = 1.0f;
    bool is_default = true; // 是否设为 Amplitude 默认监听者

    // 运行时句柄（AmListenerID，不序列化）
    uint64_t runtime_id = 0;

    AudioListener() = default;
    const char* type() const override { return "AudioListener"; }

    void serialize(nlohmann::json& out) const override {
        out["gain"] = gain;
        out["is_default"] = is_default;
    }

    void deserialize(const nlohmann::json& in) override {
        gain = in.value("gain", 1.0f);
        is_default = in.value("is_default", true);
    }
};

// ---------------------------------------------------------------------------
// AudioEmitter — 3D 音源发射器
//
// event_name 为 Amplitude 事件名；play_on_start 为 true 时在场景开始播放，
// loop 控制是否循环。运行时 playing 表示当前是否有活跃实例。
// ---------------------------------------------------------------------------
class AudioEmitter : public Component {
public:
    std::string event_name;
    bool play_on_start = true;
    bool loop = false;
    float gain = 1.0f;
    float pitch = 1.0f;
    float min_distance = 1.0f;   // 衰减起始距离
    float max_distance = 50.0f;  // 衰减终止距离（超出后听不见）

    // 运行时状态（不序列化）
    bool playing = false;
    bool start_requested = false;
    uint64_t runtime_id = 0; // AmEntityID

    AudioEmitter() = default;
    const char* type() const override { return "AudioEmitter"; }

    void serialize(nlohmann::json& out) const override {
        out["event_name"] = event_name;
        out["play_on_start"] = play_on_start;
        out["loop"] = loop;
        out["gain"] = gain;
        out["pitch"] = pitch;
        out["min_distance"] = min_distance;
        out["max_distance"] = max_distance;
    }

    void deserialize(const nlohmann::json& in) override {
        event_name = in.value("event_name", std::string{});
        play_on_start = in.value("play_on_start", true);
        loop = in.value("loop", false);
        gain = in.value("gain", 1.0f);
        pitch = in.value("pitch", 1.0f);
        min_distance = in.value("min_distance", 1.0f);
        max_distance = in.value("max_distance", 50.0f);
    }

    void snapshot_runtime_state(nlohmann::json& out) const override {
        out["playing"] = playing;
        out["start_requested"] = start_requested;
        out["runtime_id"] = runtime_id;
    }
    void restore_runtime_state(const nlohmann::json& in) override {
        playing = in.value("playing", false);
        start_requested = in.value("start_requested", false);
        runtime_id = in.value("runtime_id", static_cast<uint64_t>(0));
    }
};

} // namespace gryce_engine::components
