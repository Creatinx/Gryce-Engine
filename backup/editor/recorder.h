#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// FrameRecorder — 将每帧 RGBA 写入 PNG 序列，退出时可选调用 ffmpeg 合成 MP4。
// ---------------------------------------------------------------------------
class FrameRecorder {
public:
    explicit FrameRecorder(std::filesystem::path output_path,
                           int fps = 30,
                           bool no_audio = false);

    // 写入一帧；frame 为 top-down RGBA，大小 w*h*4。
    void write_frame(const uint8_t* rgba, int w, int h);

    // 返回已写入帧数
    int frame_count() const { return frame_count_; }

    // 完成录制：尝试用 ffmpeg 合成 MP4；失败时保留 PNG 序列。
    void finalize();

private:
    std::filesystem::path output_path_;
    std::filesystem::path work_dir_;
    int fps_ = 30;
    bool no_audio_ = false;
    int frame_count_ = 0;

    std::filesystem::path frame_path(int index) const;
    bool encode_with_ffmpeg();
};

} // namespace gryce_engine::editor
