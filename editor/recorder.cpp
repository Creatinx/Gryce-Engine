#include "recorder.h"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "stb/stb_image_write.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

FrameRecorder::FrameRecorder(std::filesystem::path output_path, int fps, bool no_audio)
    : output_path_(std::move(output_path)), fps_(fps), no_audio_(no_audio) {
    // 如果 output_path_ 带扩展名，去掉扩展名作为工作目录/输出前缀。
    std::filesystem::path base = output_path_;
    if (base.has_extension()) {
        base.replace_extension();
    }
    work_dir_ = base.parent_path() / (base.stem().string() + "_frames");

    std::error_code ec;
    std::filesystem::create_directories(work_dir_, ec);
    if (ec) {
        GLOG_ERROR("FrameRecorder: failed to create work directory '{}'", work_dir_.string());
    }
}

std::filesystem::path FrameRecorder::frame_path(int index) const {
    std::ostringstream ss;
    ss << "frame_" << std::setw(6) << std::setfill('0') << index << ".png";
    return work_dir_ / ss.str();
}

void FrameRecorder::write_frame(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    auto path = frame_path(frame_count_);
    if (stbi_write_png(path.string().c_str(), w, h, 4, rgba, w * 4)) {
        ++frame_count_;
    } else {
        GLOG_ERROR("FrameRecorder: failed to write frame '{}'", path.string());
    }
}

bool FrameRecorder::encode_with_ffmpeg() {
    if (frame_count_ == 0) return false;

    std::string ffmpeg_cmd;
#ifdef _WIN32
    ffmpeg_cmd += "ffmpeg.exe";
#else
    ffmpeg_cmd += "ffmpeg";
#endif

    // 输入帧序列
    auto input_glob = (work_dir_ / "frame_%06d.png").string();
    std::string output = output_path_.string();

    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_cmd << "\""
        << " -y -framerate " << fps_
        << " -i \"" << input_glob << "\""
        << " -c:v libx264 -pix_fmt yuv420p -preset fast -crf 18"
        << " -r " << fps_;
    if (no_audio_) {
        cmd << " -an";
    }
    cmd << " \"" << output << "\"";

    GLOG_INFO("FrameRecorder: encoding with ffmpeg: {}", cmd.str());
    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        GLOG_WARN("FrameRecorder: ffmpeg exited with code {}, leaving PNG sequence at '{}'",
                  ret, work_dir_.string());
        return false;
    }
    GLOG_INFO("FrameRecorder: MP4 saved to '{}'", output);

    // 编码成功后可清理 PNG 序列（保留工作目录或删除？这里只删除帧文件）。
    std::error_code ec;
    for (int i = 0; i < frame_count_; ++i) {
        std::filesystem::remove(frame_path(i), ec);
    }
    std::filesystem::remove(work_dir_, ec);
    return true;
}

void FrameRecorder::finalize() {
    if (frame_count_ == 0) {
        GLOG_WARN("FrameRecorder: no frames captured");
        return;
    }

    GLOG_INFO("FrameRecorder: {} frames written to '{}'", frame_count_, work_dir_.string());

    if (!encode_with_ffmpeg()) {
        GLOG_INFO("FrameRecorder: PNG sequence retained at '{}'", work_dir_.string());
    }
}

} // namespace gryce_engine::editor
