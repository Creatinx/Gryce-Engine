#include "GryceCore/asset_api.h"
#include "GryceCore/api_guard.h"
#include "internal_state.h"

#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>

using gryce_engine::assets::AssetManager;

namespace {

enum class AssetType { Mesh, Audio };

struct AssetRecord {
    std::string path;
    AssetType type = AssetType::Mesh;
    std::shared_ptr<const gryce_engine::assets::MeshData> mesh;
};

static std::mutex s_asset_mutex;
static std::unordered_map<int, AssetRecord> s_assets;
static int s_next_asset_id = 1;

// 返回小写扩展名（含点），如 ".wav"；无扩展名返回空串。
std::string file_extension(const char* path) {
    std::string p = path ? path : "";
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = p.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool is_audio_ext(const std::string& ext) {
    return ext == ".wav" || ext == ".ogg" || ext == ".mp3" ||
           ext == ".flac" || ext == ".aac" || ext == ".wma" || ext == ".m4a";
}

// 登记一个资源到资产表，返回其句柄。音频按扩展名识别并以路径登记
// （AudioSource::clip_path 直接按路径加载，不占用 mesh 缓存）；其余走网格导入。
GAssetHandle register_asset(const char* source_path, const char* what) {
    GRYCE_API_GUARD();
    if (!source_path || !source_path[0]) return -1;

    std::lock_guard lock(s_asset_mutex);
    auto& mgr = AssetManager::instance();

    AssetRecord rec;
    rec.path = source_path;

    std::string ext = file_extension(source_path);
    if (is_audio_ext(ext)) {
        rec.type = AssetType::Audio;
    } else {
        auto mesh = mgr.load_mesh_shared(source_path);
        if (!mesh) return -1;
        rec.type = AssetType::Mesh;
        rec.mesh = std::move(mesh);
    }

    int id = s_next_asset_id++;
    s_assets[id] = std::move(rec);
    GLOG_INFO("GAsset_{}: '{}' -> handle {}", what, source_path, id);
    return id;
}

} // namespace

extern "C" {

GAssetHandle GAsset_Import(const char* source_path) {
    return register_asset(source_path, "Import");
}

GAssetHandle GAsset_Load(const char* path) {
    return register_asset(path, "Load");
}

int GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (handle <= 0 || !out_buf || buf_size <= 0) return -1;

    std::lock_guard lock(s_asset_mutex);
    auto it = s_assets.find(handle);
    if (it == s_assets.end()) return -1;

    std::strncpy(out_buf, it->second.path.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

void GAsset_Unload(GAssetHandle handle) {
    GRYCE_API_GUARD();
    if (handle <= 0) return;

    std::lock_guard lock(s_asset_mutex);
    auto it = s_assets.find(handle);
    if (it != s_assets.end()) {
        AssetManager::instance().unload(it->second.path);
        s_assets.erase(it);
    }
}

} // extern "C"
