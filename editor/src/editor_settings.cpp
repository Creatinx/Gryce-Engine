#include "editor_settings.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace gryce_engine::editor {

EditorSettings& EditorSettings::instance() {
    static EditorSettings settings;
    return settings;
}

std::string EditorSettings::config_path() const {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0]) {
        return (std::string(appdata) + "/GryceEngine/editor_settings.json");
    }
#endif
    // 非 Windows / 无 APPDATA 时回落当前目录。
    return "editor_settings.json";
}

void EditorSettings::load() {
    std::ifstream in(config_path());
    if (!in.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains("window_width") && j["window_width"].is_number_integer()) {
            window_width_ = std::max(320, j["window_width"].get<int>());
        }
        if (j.contains("window_height") && j["window_height"].is_number_integer()) {
            window_height_ = std::max(240, j["window_height"].get<int>());
        }
        if (j.contains("window_maximized") && j["window_maximized"].is_boolean()) {
            window_maximized_ = j["window_maximized"].get<bool>();
        }
        if (j.contains("theme") && j["theme"].is_number_integer()) {
            // 迁移旧版本主题数值：
            //  旧 BlenderDark=0 -> 新 Dark=0
            //  旧 Godot4Dark=1  -> 新 Dark=0（该主题已删除，回落到默认暗色）
            //  旧 Light=2       -> 新 Light=1
            const int theme = j["theme"].get<int>();
            switch (theme) {
            case 0: theme_ = EditorTheme::Dark; break;  // 旧 BlenderDark / 残留值
            case 1: theme_ = EditorTheme::Dark; break;  // 旧 Godot4Dark（已删除）
            case 2: theme_ = EditorTheme::Light; break; // 旧 Light
            default: theme_ = EditorTheme::Dark; break; // 未知值回落
            }
        }
        if (j.contains("language") && j["language"].is_string()) {
            language_ = j["language"].get<std::string>();
        }
    } catch (...) {
        // 解析失败则保持默认值，不覆盖现有运行态偏好。
    }
}

void EditorSettings::save() {
    nlohmann::json j;
    j["window_width"] = window_width_;
    j["window_height"] = window_height_;
    j["window_maximized"] = window_maximized_;
    j["theme"] = static_cast<int>(theme_);
    j["language"] = language_;

    const std::string path = config_path();
#ifdef _WIN32
    // 确保父目录存在。
    const std::string parent = path.substr(0, path.find_last_of('/'));
    if (!parent.empty() && parent != path) {
        std::wstring wide(parent.begin(), parent.end());
        CreateDirectoryW(wide.c_str(), nullptr);
    }
#endif
    std::ofstream out(path);
    if (out.is_open()) {
        out << j.dump(2);
    }
}

void EditorSettings::set_window_width(int v) {
    window_width_ = std::max(320, v);
}

void EditorSettings::set_window_height(int v) {
    window_height_ = std::max(240, v);
}

void EditorSettings::set_window_maximized(bool v) {
    window_maximized_ = v;
}

void EditorSettings::set_theme(EditorTheme v) {
    theme_ = v;
}

void EditorSettings::set_language(const std::string& v) {
    language_ = v;
}

} // namespace gryce_engine::editor