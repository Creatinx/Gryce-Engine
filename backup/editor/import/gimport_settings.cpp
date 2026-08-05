#include "gimport_settings.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

std::string gimport_path_for(const std::string& source_path) {
    return source_path + ".gimport";
}

} // namespace

GImportSettings load_gimport_settings(const std::string& source_path) {
    GImportSettings settings;

    std::string path = gimport_path_for(source_path);
    if (!std::filesystem::exists(path)) {
        return settings;
    }

    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("GImportSettings: failed to open '{}'", path);
        return settings;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("model")) {
            const auto& model = j["model"];
            settings.scale = model.value("scale", 1.0f);
            settings.generate_collider = model.value("generate_collider", true);
            settings.add_rigidbody = model.value("add_rigidbody", false);
            settings.physics_material = model.value("physics_material", "");
        }
    } catch (const std::exception& e) {
        GLOG_ERROR("GImportSettings: failed to parse '{}': {}", path, e.what());
    }

    return settings;
}

void save_gimport_settings(const std::string& source_path, const GImportSettings& settings) {
    nlohmann::json j;
    j["model"]["scale"] = settings.scale;
    j["model"]["generate_collider"] = settings.generate_collider;
    j["model"]["add_rigidbody"] = settings.add_rigidbody;
    j["model"]["physics_material"] = settings.physics_material;

    std::string path = gimport_path_for(source_path);
    std::ofstream ofs(path);
    if (!ofs) {
        GLOG_ERROR("GImportSettings: failed to write '{}'", path);
        return;
    }
    ofs << j.dump(4);
    GLOG_INFO("GImportSettings: saved '{}'", path);
}

GImportSettings ensure_gimport_settings(const std::string& source_path) {
    std::string path = gimport_path_for(source_path);
    if (!std::filesystem::exists(path)) {
        GImportSettings defaults;
        save_gimport_settings(source_path, defaults);
        return defaults;
    }
    return load_gimport_settings(source_path);
}

} // namespace gryce_engine::editor
