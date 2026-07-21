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
            settings.render_api = render_api_from_string(graphics.value("render_api", "opengl"));
        }
    } catch (const std::exception& e) {
        GLOG_ERROR("ProjectSettings: failed to parse '{}': {}", path, e.what());
    }

    return settings;
}

void save_project_settings(const std::string& project_root, const ProjectSettings& settings) {
    nlohmann::json j;
    j["graphics"]["render_api"] = render_api_to_string(settings.render_api);

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
        case render::RenderAPI::OpenGL:
        default: return "opengl";
    }
}

render::RenderAPI render_api_from_string(const std::string& s) {
    if (s == "vulkan") return render::RenderAPI::Vulkan;
    return render::RenderAPI::OpenGL;
}

} // namespace gryce_engine::editor
