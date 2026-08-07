#include "GryceCore/asset_api.h"
#include "internal_state.h"

#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>

using gryce_engine::assets::AssetManager;

namespace {

struct AssetRecord {
    std::string path;
    std::shared_ptr<const gryce_engine::assets::MeshData> mesh;
};

static std::mutex s_asset_mutex;
static std::unordered_map<int, AssetRecord> s_assets;
static int s_next_asset_id = 1;

} // namespace

extern "C" {

GAssetHandle GAsset_Import(const char* source_path) {
    if (!source_path || !source_path[0]) return -1;

    std::lock_guard lock(s_asset_mutex);
    auto& mgr = AssetManager::instance();
    auto mesh = mgr.load_mesh_shared(source_path);
    if (mesh) {
        int id = s_next_asset_id++;
        AssetRecord rec;
        rec.path = source_path;
        rec.mesh = std::move(mesh);
        s_assets[id] = std::move(rec);
        GLOG_INFO("GAsset_Import: loaded '{}' -> handle {}", source_path, id);
        return id;
    }
    return -1;
}

GAssetHandle GAsset_Load(const char* path) {
    if (!path || !path[0]) return -1;

    std::lock_guard lock(s_asset_mutex);
    auto& mgr = AssetManager::instance();
    auto mesh = mgr.load_mesh_shared(path);
    if (mesh) {
        int id = s_next_asset_id++;
        AssetRecord rec;
        rec.path = path;
        rec.mesh = std::move(mesh);
        s_assets[id] = std::move(rec);
        GLOG_INFO("GAsset_Load: loaded '{}' -> handle {}", path, id);
        return id;
    }
    return -1;
}

int GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size) {
    if (handle <= 0 || !out_buf || buf_size <= 0) return -1;

    std::lock_guard lock(s_asset_mutex);
    auto it = s_assets.find(handle);
    if (it == s_assets.end()) return -1;

    std::strncpy(out_buf, it->second.path.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

void GAsset_Unload(GAssetHandle handle) {
    if (handle <= 0) return;

    std::lock_guard lock(s_asset_mutex);
    auto it = s_assets.find(handle);
    if (it != s_assets.end()) {
        AssetManager::instance().unload(it->second.path);
        s_assets.erase(it);
    }
}

} // extern "C"