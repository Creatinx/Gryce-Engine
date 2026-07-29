#include "cli_args.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

namespace gryce_engine::editor {

namespace {

bool arg_matches(const char* arg, const char* name) {
    return std::strcmp(arg, name) == 0;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_resolution(const char* value, int& w, int& h) {
    std::string s(value);
    auto x_pos = s.find('x');
    if (x_pos == std::string::npos) {
        x_pos = s.find('X');
    }
    if (x_pos == std::string::npos || x_pos == 0 || x_pos + 1 >= s.size()) {
        return false;
    }
    try {
        w = std::stoi(s.substr(0, x_pos));
        h = std::stoi(s.substr(x_pos + 1));
        return w > 0 && h > 0;
    } catch (...) {
        return false;
    }
}

CameraPreset parse_camera_preset(const char* value) {
    const std::string s = to_lower(value);
    if (s == "orbit") return CameraPreset::Orbit;
    if (s == "flythrough") return CameraPreset::Flythrough;
    if (s == "demo") return CameraPreset::Demo;
    return CameraPreset::Static;
}

void print_help(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --scene <scene_name>      Load scene file (.gesc)\n"
              << "  --record <seconds>        Record N seconds video, then exit\n"
              << "  --output <path>           Output path for recording\n"
              << "  --resolution <WxH>        Window resolution (default 1920x1080)\n"
              << "  --camera <preset>         Camera preset: orbit | flythrough | static | demo\n"
              << "  --vulkan                  Use Vulkan backend (default)\n"
              << "  --opengl                  Use OpenGL backend (compatibility)\n"
              << "  --vulkan-validation       Enable Vulkan validation layers\n"
              << "  --headless                Run without visible window (best-effort)\n"
              << "  --no-audio                Do not record system audio\n"
              << "  --help                    Show this help message\n";
}

} // namespace

CliArgs parse_cli_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg_matches(arg, "--help")) {
            args.show_help = true;
            print_help(argc > 0 ? argv[0] : "gryce-engine");
            return args;
        } else if (arg_matches(arg, "--scene")) {
            if (i + 1 < argc) args.scene_path = argv[++i];
        } else if (arg_matches(arg, "--record")) {
            if (i + 1 < argc) {
                try {
                    args.record_seconds = std::stof(argv[++i]);
                    if (args.record_seconds < 0.0f) args.record_seconds = 0.0f;
                } catch (...) {
                    args.record_seconds = 0.0f;
                }
            }
        } else if (arg_matches(arg, "--output")) {
            if (i + 1 < argc) args.output_path = argv[++i];
        } else if (arg_matches(arg, "--resolution")) {
            if (i + 1 < argc) {
                if (!parse_resolution(argv[++i], args.resolution_w, args.resolution_h)) {
                    args.resolution_w = 1920;
                    args.resolution_h = 1080;
                }
            }
        } else if (arg_matches(arg, "--camera")) {
            if (i + 1 < argc) args.camera_preset = parse_camera_preset(argv[++i]);
        } else if (arg_matches(arg, "--headless")) {
            args.headless = true;
        } else if (arg_matches(arg, "--no-audio")) {
            args.no_audio = true;
        }
    }
    return args;
}

} // namespace gryce_engine::editor
