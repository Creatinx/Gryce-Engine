#pragma once

// GryceEngineUtils::audio.h — 可选音频模块入口
//
// AudioEngine 是音频引擎（miniaudio）的独立包装，不依赖 ECS。

#include "GryceEngineUtils/math.h"

namespace GryceEngineUtils {

class AudioEngine {
public:
    static AudioEngine* create();
    void destroy();

    // 后续添加：play_sound, set_volume, set_listener 等
    void set_listener_position(const math::Vector3f& pos);

private:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
};

} // namespace GryceEngineUtils
