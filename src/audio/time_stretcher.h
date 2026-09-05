#pragma once

#include <cstdint>
#include <vector>

namespace gryce_engine::audio {

// ---------------------------------------------------------------------------
// TimeStretcher — 轻量离线时域拉伸（OLA）。
// 改变播放速度同时尽量保持音调不变。
// 适用于短音效；长音频/流式音频仍回退到 miniaudio 原生 pitch。
// ---------------------------------------------------------------------------
class TimeStretcher {
public:
    // speed: 1.0=原速, 0.5=半速, 2.0=倍速
    // input/output 均为交错 float PCM（范围 [-1,1]）
    static bool process(const float* input, int64_t input_frames, int channels,
                        float speed, std::vector<float>& output);
};

} // namespace gryce_engine::audio
