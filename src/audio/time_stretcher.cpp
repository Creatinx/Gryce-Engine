#include "audio/time_stretcher.h"

#include <cmath>
#include <algorithm>

namespace gryce_engine::audio {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Hann 窗，满足 COLA 条件（当 analysis_hop == synthesis_hop 时完美重建）
void hann_window(int size, std::vector<float>& out) {
    out.resize(size);
    if (size <= 1) {
        out[0] = 1.0f;
        return;
    }
    for (int i = 0; i < size; ++i) {
        out[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(size - 1));
    }
}

} // namespace

bool TimeStretcher::process(const float* input, int64_t input_frames, int channels,
                            float speed, std::vector<float>& output) {
    if (!input || input_frames <= 0 || channels <= 0 || speed <= 0.0f) {
        return false;
    }

    // 接近原速时直接复制，避免无谓的 artifacts
    if (std::abs(speed - 1.0f) < 0.005f) {
        output.assign(input, input + input_frames * channels);
        return true;
    }

    // 参数：帧长与跳步。帧长越大低频保留越好，延迟越大。
    constexpr int kFrameSize = 2048;
    constexpr int kAnalysisHop = kFrameSize / 4;

    const int frame_size = kFrameSize;
    const int analysis_hop = kAnalysisHop;
    const float synthesis_hop_f = static_cast<float>(analysis_hop) / speed;
    const int synthesis_hop = std::max(1, static_cast<int>(synthesis_hop_f + 0.5f));

    // 预计算窗函数
    std::vector<float> window;
    hann_window(frame_size, window);

    // 估算输出帧数并分配空间（多留一帧余量）
    const int64_t estimated_output_frames =
        static_cast<int64_t>(static_cast<float>(input_frames) / speed) + frame_size;
    output.assign(estimated_output_frames * channels, 0.0f);

    std::vector<float> coverage(estimated_output_frames * channels, 0.0f);

    int64_t output_pos = 0;
    for (int64_t input_pos = 0; input_pos + frame_size <= input_frames; input_pos += analysis_hop) {
        for (int i = 0; i < frame_size; ++i) {
            const int64_t out_idx_base = (output_pos + i) * channels;
            if (out_idx_base + channels > static_cast<int64_t>(output.size())) break;
            const float w = window[i];
            for (int ch = 0; ch < channels; ++ch) {
                const int64_t idx = out_idx_base + ch;
                output[idx] += input[(input_pos + i) * channels + ch] * w;
                coverage[idx] += w;
            }
        }
        output_pos += synthesis_hop;
    }

    // 归一化：按窗叠加和除，避免振幅调制
    const int64_t total_samples = output_pos * channels;
    for (int64_t i = 0; i < total_samples; ++i) {
        if (coverage[i] > 1e-6f) {
            output[i] /= coverage[i];
        }
    }

    // 截断到实际长度
    output.resize(total_samples);
    return true;
}

} // namespace gryce_engine::audio
