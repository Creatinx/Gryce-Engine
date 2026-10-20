#include "api/asset_api.h"
#include "api/api_guard.h"
#include "runtime/engine_context.h"

#include "assets/asset_manager.h"
#include "assets/asset_meta.h"
#include "assets/mesh_data.h"
#include "resources/resource_path.h"
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

// 定长缓冲拷贝（超长截断，保证以 '\0' 结尾）。
void copy_cstr(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) return;
    std::strncpy(dst, src.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// AssetMeta ↔ GAssetMetaInfo（C API 的 POD 镜像）。
void fill_meta_info(GAssetMetaInfo& info, const gryce_engine::assets::AssetMeta& meta) {
    copy_cstr(info.guid, sizeof(info.guid), meta.guid);
    copy_cstr(info.source_path, sizeof(info.source_path), meta.source_path);
    copy_cstr(info.importer, sizeof(info.importer), meta.importer);
    info.scale = meta.scale;
    info.up_axis = meta.up_axis;
    info.generate_lod = meta.generate_lod ? 1 : 0;
    info.import_materials = meta.import_materials ? 1 : 0;
    info.roughness_scale = meta.roughness_scale;
    info.version = meta.version;
}

void read_meta_info(gryce_engine::assets::AssetMeta& meta, const GAssetMetaInfo& info) {
    meta.guid = info.guid;
    meta.source_path = info.source_path;
    meta.importer = info.importer;
    meta.scale = info.scale;
    meta.up_axis = info.up_axis;
    meta.generate_lod = info.generate_lod != 0;
    meta.import_materials = info.import_materials != 0;
    meta.roughness_scale = info.roughness_scale;
}

} // namespace

extern "C" {

GAssetHandle GAsset_Import(const char* source_path) {
    const GAssetHandle handle = register_asset(source_path, "Import");
    // 显式导入才建立侧车文件：普通加载（GAsset_Load）不应该往工程目录写东西。
    if (handle > 0 && source_path && source_path[0]) {
        // res:/ 路径必须先落到真实文件，否则会写出一个名为 "res:" 的目录。
        std::string resolved = source_path;
        if (resolved.rfind("res:/", 0) == 0) {
            resolved = gryce_engine::resources::ResourcePath::resolve(resolved);
        }
        gryce_engine::assets::AssetMeta meta;
        gryce_engine::assets::load_or_create_asset_meta(resolved, meta);
    }
    return handle;
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

// ---------------------------------------------------------------------------
// 侧车文件（<asset>.meta）
// ---------------------------------------------------------------------------

int GAsset_ReadMeta(const char* asset_path, GAssetMetaInfo* out) {
    GRYCE_API_GUARD();
    if (!asset_path || !asset_path[0] || !out) return 0;
    gryce_engine::assets::AssetMeta meta;
    if (!gryce_engine::assets::read_asset_meta(asset_path, meta)) return 0;
    fill_meta_info(*out, meta);
    return 1;
}

int GAsset_EnsureMeta(const char* asset_path, GAssetMetaInfo* out) {
    GRYCE_API_GUARD();
    if (!asset_path || !asset_path[0] || !out) return 0;
    gryce_engine::assets::AssetMeta meta;
    if (!gryce_engine::assets::load_or_create_asset_meta(asset_path, meta)) return 0;
    if (out) fill_meta_info(*out, meta);
    return 1;
}

int GAsset_WriteMeta(const char* asset_path, const GAssetMetaInfo* info) {
    GRYCE_API_GUARD();
    if (!asset_path || !asset_path[0] || !info) return 0;
    gryce_engine::assets::AssetMeta meta;
    // 先读现有侧车再覆盖：调用方通常只改一两个字段，其余保持原值；
    // 不存在时用默认值作为基底。
    gryce_engine::assets::read_asset_meta(asset_path, meta);
    read_meta_info(meta, *info);
    if (meta.guid.empty()) meta.guid = gryce_engine::assets::generate_asset_guid();
    if (meta.importer.empty()) meta.importer = "mesh";
    if (!gryce_engine::assets::asset_meta_validate(meta)) {
        GLOG_WARN("GAsset_WriteMeta: '{}' 导入设置非法，拒绝写入", asset_path);
        return 0;
    }
    if (!gryce_engine::assets::write_asset_meta(asset_path, meta)) return 0;
    // 导入设置变了，缓存里的旧网格必须重导，否则界面改了设置画面不变。
    AssetManager::instance().unload_mesh(asset_path);
    GLOG_INFO("GAsset_WriteMeta: '{}' 已更新（scale={} up_axis={}）",
              asset_path, meta.scale, meta.up_axis);
    return 1;
}

int GAsset_IsMetaStale(const char* asset_path) {
    GRYCE_API_GUARD();
    if (!asset_path || !asset_path[0]) return 0;
    gryce_engine::assets::AssetMeta meta;
    if (!gryce_engine::assets::read_asset_meta(asset_path, meta)) return 0;
    return gryce_engine::assets::asset_meta_is_stale(asset_path, meta) ? 1 : 0;
}

} // extern "C"
