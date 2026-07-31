#include "editor_theme.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

std::string theme_config_path(const std::string& project_root) {
    return project_root + "/editor_theme.json";
}

} // namespace

ThemeConfig default_theme_config() {
    return ThemeConfig{};
}

bool load_editor_font(const ThemeConfig& config) {
    return EngineTheme::LoadFonts(nullptr, 14.0f * config.ui_scale,
                                  nullptr, 13.0f * config.ui_scale);
}

void apply_theme(ThemePreset preset, const ThemeConfig& config) {
    // Dark/Light 预设即 Xcode 暗色/亮色主题
    if (preset == ThemePreset::Dark) {
        apply_xcode_dark();
    } else if (preset == ThemePreset::Light) {
        apply_xcode_light();
    } else {
        EngineTheme::ModernLight::Apply();
    }

    const float safe_scale = std::max(config.ui_scale, 0.5f);
    EngineTheme::ScaleStyle(safe_scale);

    if (ImGui::GetCurrentContext()) {
        if (preset == ThemePreset::ModernLight) {
            // ModernLight 与 Light 使用相同基础字号 14px，避免整体大小差异过大
            EngineTheme::LoadModernFonts(14.0f * safe_scale,
                                         13.0f * safe_scale);
        } else {
            ThemeConfig scaled_config = config;
            scaled_config.ui_scale = safe_scale;
            load_editor_font(scaled_config);
        }
    }
}

void save_theme_config(const std::string& project_root, const ThemeConfig& config, ThemePreset preset) {
    nlohmann::json j;
    std::string preset_name = "light";
    if (preset == ThemePreset::Dark)        preset_name = "dark";
    else if (preset == ThemePreset::ModernLight) preset_name = "modern_light";
    j["preset"]      = preset_name;
    j["font_size"]   = config.font_size;
    j["ui_scale"]    = config.ui_scale;

    std::string path = theme_config_path(project_root);
    std::ofstream ofs(path);
    if (!ofs) {
        GLOG_ERROR("EditorTheme: failed to write '{}'", path);
        return;
    }
    ofs << j.dump(4);
    GLOG_INFO("EditorTheme: saved config '{}'", path);
}

bool load_theme_config(const std::string& project_root, ThemeConfig& out_config, ThemePreset& out_preset) {
    std::string path = theme_config_path(project_root);
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("EditorTheme: failed to open '{}'", path);
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        out_config.font_size = j.value("font_size", out_config.font_size);
        out_config.ui_scale  = j.value("ui_scale",  out_config.ui_scale);
        if (out_config.ui_scale <= 0.0f || out_config.ui_scale > 3.0f) {
            out_config.ui_scale = EngineTheme::k_default_ui_scale;
        }
        std::string preset_str = j.value("preset", "dark");
        if (preset_str == "light")        out_preset = ThemePreset::Light;
        else if (preset_str == "modern_light") out_preset = ThemePreset::ModernLight;
        // 兼容旧配置："xcode" 预设已并入 Dark
        else                              out_preset = ThemePreset::Dark;
    } catch (const std::exception& e) {
        GLOG_ERROR("EditorTheme: failed to parse '{}': {}", path, e.what());
        return false;
    }
    GLOG_INFO("EditorTheme: loaded config '{}'", path);
    return true;
}

} // namespace gryce_engine::editor
