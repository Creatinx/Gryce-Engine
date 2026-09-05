#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// AudioListener — 音频监听器组件。
// 标记当前 Entity 的位置为 3D 音频的监听点。
// 场景中通常只有一个 AudioListener（挂载到主相机或玩家实体）。
// ---------------------------------------------------------------------------
class AudioListener : public Component {
public:
    // 全局音量缩放（不影响 AudioSource 的独立 volume）
    float global_volume = 1.0f;

    AudioListener() = default;

    const char* type() const override { return "AudioListener"; }

    void serialize(nlohmann::json& out) const override {
        out["global_volume"] = global_volume;
    }

    void deserialize(const nlohmann::json& in) override {
        global_volume = in.value("global_volume", 1.0f);
    }
};

} // namespace gryce_engine::components
