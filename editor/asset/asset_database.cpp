#include "asset_database.h"

#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

AssetDatabase& AssetDatabase::instance() {
    static AssetDatabase db;
    return db;
}

std::filesystem::path AssetDatabase::meta_path_for(const std::filesystem::path& file_path) {
    return file_path.string() + ".meta";
}

std::string AssetDatabase::generate_guid() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";

    std::string guid;
    guid.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            guid.push_back('-');
        } else {
            guid.push_back(hex[dist(rng)]);
        }
    }
    return guid;
}

std::string AssetDatabase::infer_type(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
        ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2") {
        return "texture";
    }
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" ||
        ext == ".dae" || ext == ".ply" || ext == ".stl") {
        return "mesh";
    }
    if (ext == ".gmat") return "material";
    if (ext == ".gesc") return "scene";
    if (ext == ".geprefab") return "prefab";
    if (ext == ".geprefabvariant") return "prefab_variant";
    if (ext == ".gimport") return "import_settings";
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return "audio";
    if (ext == ".ttf" || ext == ".otf") return "font";
    return "unknown";
}

void AssetDatabase::load_or_create_meta(const std::filesystem::path& file_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file_path, ec) || ec) return;
    if (file_path.extension() == ".meta") return;

    std::filesystem::path meta = meta_path_for(file_path);
    std::string guid;
    std::string type = infer_type(file_path);

    if (std::filesystem::exists(meta, ec) && !ec) {
        std::ifstream ifs(meta);
        if (ifs) {
            try {
                nlohmann::json j = nlohmann::json::parse(ifs);
                guid = j.value("guid", "");
                type = j.value("type", type);
            } catch (const std::exception& e) {
                GLOG_WARN("AssetDatabase: failed to parse '{}': {}", meta.string(), e.what());
            }
        }
    }

    if (guid.empty()) {
        guid = generate_guid();
        nlohmann::json j;
        j["guid"] = guid;
        j["type"] = type;
        std::ofstream ofs(meta);
        if (ofs) {
            ofs << j.dump(2);
            GLOG_INFO("AssetDatabase: created meta '{}' for '{}'", meta.string(), file_path.string());
        } else {
            GLOG_ERROR("AssetDatabase: failed to write meta '{}'", meta.string());
        }
    }

    std::string canonical = std::filesystem::canonical(file_path, ec).string();
    if (ec) canonical = file_path.string();

    guid_to_path_[guid] = canonical;
    path_to_guid_[canonical] = guid;
}

void AssetDatabase::scan(const std::filesystem::path& project_root) {
    root_ = project_root;
    std::error_code ec;

    std::filesystem::path assets_dir = project_root / "assets";
    if (!std::filesystem::exists(assets_dir, ec) || ec) {
        assets_dir = project_root;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(assets_dir, ec)) {
        if (!entry.is_regular_file(ec) || ec) continue;
        load_or_create_meta(entry.path());
    }
}

void AssetDatabase::rescan(const std::filesystem::path& project_root) {
    guid_to_path_.clear();
    path_to_guid_.clear();
    scan(project_root);
}

std::string AssetDatabase::ensure_meta(const std::filesystem::path& file_path) {
    std::error_code ec;
    std::string canonical = std::filesystem::canonical(file_path, ec).string();
    if (ec) canonical = file_path.string();

    auto it = path_to_guid_.find(canonical);
    if (it != path_to_guid_.end()) return it->second;

    load_or_create_meta(file_path);
    return path_to_guid_[canonical];
}

std::string AssetDatabase::guid_for_path(const std::filesystem::path& path) const {
    std::error_code ec;
    std::string canonical = std::filesystem::canonical(path, ec).string();
    if (ec) canonical = path.string();

    auto it = path_to_guid_.find(canonical);
    if (it != path_to_guid_.end()) return it->second;
    return "";
}

std::string AssetDatabase::path_for_guid(const std::string& guid) const {
    auto it = guid_to_path_.find(guid);
    if (it != guid_to_path_.end()) return it->second;
    return "";
}

} // namespace gryce_engine::editor
