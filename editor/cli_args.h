#pragma once

#include <string>
#include <vector>

#include "camera_preset.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// CliArgs — 编辑器命令行参数解析结果
// ---------------------------------------------------------------------------

struct CliArgs {
    std::string scene_path;          // --scene <scene_name> (.gesc)
    std::string output_path;         // --output <path>
    float record_seconds = 0.0f;     // --record <seconds>; 0 表示不录制
    int resolution_w = 1920;         // --resolution <WxH>
    int resolution_h = 1080;
    CameraPreset camera_preset = CameraPreset::Static; // --camera <preset>
    bool headless = false;           // --headless
    bool no_audio = false;           // --no-audio
    bool show_help = false;          // --help

    bool should_record() const { return record_seconds > 0.0f; }
};

CliArgs parse_cli_args(int argc, char* argv[]);

} // namespace gryce_engine::editor
