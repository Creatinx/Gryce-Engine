#include "recorder.h"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

#include "stb/stb_image_write.h"

#include "utils/glog/glog_lib.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

namespace {

#ifdef _WIN32
// 把 UTF-8 字符串转成宽字符，用于 Windows API。
std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), result.data(), size);
    return result;
}

// 按 argv 方式构造命令行，按 Windows 规则对每个参数加引号/转义。
// 规则：参数首尾加 "，内部已有的 " 转义成 \\"。
std::wstring build_command_line(const std::vector<std::string>& argv) {
    std::wstring cmd_line;
    for (const std::string& arg : argv) {
        std::wstring warg = utf8_to_wstring(arg);
        if (!cmd_line.empty()) cmd_line.push_back(L' ');
        cmd_line.push_back(L'"');
        for (wchar_t c : warg) {
            if (c == L'"') {
                cmd_line.push_back(L'\\');
                cmd_line.push_back(L'"');
            } else {
                cmd_line.push_back(c);
            }
        }
        cmd_line.push_back(L'"');
    }
    return cmd_line;
}

bool launch_ffmpeg(const std::vector<std::string>& argv) {
    std::wstring cmd_line = build_command_line(argv);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                  nullptr, &si, &pi);
    if (!created) {
        GLOG_ERROR("FrameRecorder: failed to create ffmpeg process (error {})", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code) == 0;
}
#else
bool launch_ffmpeg(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const std::string& arg : argv) {
        if (!cmd.empty()) cmd.push_back(' ');
        cmd.push_back('\'');
        for (char c : arg) {
            if (c == '\'') {
                cmd += "'\\''";
            } else {
                cmd.push_back(c);
            }
        }
        cmd.push_back('\'');
    }
    int ret = std::system(cmd.c_str());
    return ret == 0;
}
#endif

} // namespace

bool FrameRecorder::encode_with_ffmpeg() {
    if (frame_count_ == 0) return false;

    std::string ffmpeg_cmd;
#ifdef _WIN32
    ffmpeg_cmd += "ffmpeg.exe";
#else
    ffmpeg_cmd += "ffmpeg";
#endif

    // 输入帧序列
    std::string input_glob = (work_dir_ / "frame_%06d.png").string();
    std::string output = output_path_.string();

    std::vector<std::string> argv;
    argv.push_back(ffmpeg_cmd);
    argv.push_back("-y");
    argv.push_back("-framerate");
    argv.push_back(std::to_string(fps_));
    argv.push_back("-i");
    argv.push_back(input_glob);
    argv.push_back("-c:v");
    argv.push_back("libx264");
    argv.push_back("-pix_fmt");
    argv.push_back("yuv420p");
    argv.push_back("-preset");
    argv.push_back("fast");
    argv.push_back("-crf");
    argv.push_back("18");
    argv.push_back("-r");
    argv.push_back(std::to_string(fps_));
    if (no_audio_) {
        argv.push_back("-an");
    }
    argv.push_back(output);

    GLOG_INFO("FrameRecorder: encoding with ffmpeg: {}", ffmpeg_cmd);
    bool ok = launch_ffmpeg(argv);
    if (!ok) {
        GLOG_WARN("FrameRecorder: ffmpeg failed, leaving PNG sequence at '{}'", work_dir_.string());
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
