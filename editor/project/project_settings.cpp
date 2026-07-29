#include "project_settings.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

std::string project_settings_json_path(const std::string& project_root) {
    return project_root + "/project_settings.json";
}

} // namespace

ProjectSettings load_project_settings(const std::string& project_root) {
    ProjectSettings settings;

    std::string path = project_settings_json_path(project_root);
    if (!std::filesystem::exists(path)) {
        return settings;
    }

    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("ProjectSettings: failed to open '{}'", path);
        return settings;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("graphics")) {
            const auto& graphics = j["graphics"];
            settings.render_api = render_api_from_string(graphics.value("render_api", "vulkan"));
            auto& q = settings.quality;
            q.shadow_map_size = graphics.value("shadow_map_size", q.shadow_map_size);
            q.shadow_bias = graphics.value("shadow_bias", q.shadow_bias);
            q.shadow_area = graphics.value("shadow_area", q.shadow_area);
            if (graphics.contains("ambient") && graphics["ambient"].is_array() &&
                graphics["ambient"].size() == 3) {
                for (int i = 0; i < 3; ++i) q.ambient[i] = graphics["ambient"][i].get<float>();
            }
            q.hdr_enabled = graphics.value("hdr_enabled", q.hdr_enabled);
            q.exposure = graphics.value("exposure", q.exposure);
            q.tone_map_mode = graphics.value("tone_map_mode", q.tone_map_mode);
            q.ibl_intensity = graphics.value("ibl_intensity", q.ibl_intensity);
        }
    } catch (const std::exception& e) {
        GLOG_ERROR("ProjectSettings: failed to parse '{}': {}", path, e.what());
    }

    return settings;
}

void save_project_settings(const std::string& project_root, const ProjectSettings& settings) {
    nlohmann::json j;
    auto& g = j["graphics"];
    g["render_api"] = render_api_to_string(settings.render_api);
    const auto& q = settings.quality;
    g["shadow_map_size"] = q.shadow_map_size;
    g["shadow_bias"] = q.shadow_bias;
    g["shadow_area"] = q.shadow_area;
    g["ambient"] = {q.ambient[0], q.ambient[1], q.ambient[2]};
    g["hdr_enabled"] = q.hdr_enabled;
    g["exposure"] = q.exposure;
    g["tone_map_mode"] = q.tone_map_mode;
    g["ibl_intensity"] = q.ibl_intensity;

    std::string path = project_settings_json_path(project_root);
    std::ofstream ofs(path);
    if (!ofs) {
        GLOG_ERROR("ProjectSettings: failed to write '{}'", path);
        return;
    }
    ofs << j.dump(4);
    GLOG_INFO("ProjectSettings: saved settings '{}'", path);
}

std::string render_api_to_string(render::RenderAPI api) {
    switch (api) {
        case render::RenderAPI::Vulkan: return "vulkan";
        case render::RenderAPI::OpenGL: return "opengl";
        case render::RenderAPI::DX11: return "dx11";
        case render::RenderAPI::DX12: return "dx12";
        default: return "vulkan";
    }
}

render::RenderAPI render_api_from_string(const std::string& s) {
    if (s == "opengl") return render::RenderAPI::OpenGL;
    if (s == "dx11") return render::RenderAPI::DX11;
    if (s == "dx12") return render::RenderAPI::DX12;
    return render::RenderAPI::Vulkan;  // 未知/空值回退默认后端
}

} // namespace gryce_engine::editor
