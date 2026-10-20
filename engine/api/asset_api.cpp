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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

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

// ---------------------------------------------------------------------------
// GAsset_ScanDirectory 的辅助
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

// JSON 字符串转义。资产文件名里可能出现引号、反斜杠（Windows 路径）与中文。
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// 侧车文件不是资产本身，列举时必须排除，否则列表里每个资产都会多出一条 .meta。
bool is_meta_file(const std::string& name) {
    constexpr const char* kSuffix = ".meta";
    constexpr std::size_t kLen = 5;
    return name.size() > kLen &&
           name.compare(name.size() - kLen, kLen, kSuffix) == 0;
}

// 扩展名 → 列表分组。只决定界面上归到哪一类，不代表可导入；
// 真正能否导入仍由 AssetManager::import_mesh_file 决定。
const char* asset_kind(const std::string& ext) {
    static const char* kMesh[] = {".obj", ".fbx", ".gltf", ".glb", ".dae",
                                  ".ply", ".stl", ".3ds", ".blend"};
    static const char* kTexture[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp",
                                     ".hdr", ".dds", ".ktx", ".ktx2", ".exr"};
    static const char* kAudio[] = {".wav", ".ogg", ".mp3", ".flac", ".aac",
                                   ".wma", ".m4a"};
    auto has = [&](const char* const* table, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (ext == table[i]) return true;
        }
        return false;
    };
    if (has(kMesh, sizeof(kMesh) / sizeof(kMesh[0]))) return "mesh";
    if (has(kTexture, sizeof(kTexture) / sizeof(kTexture[0]))) return "texture";
    if (has(kAudio, sizeof(kAudio) / sizeof(kAudio[0]))) return "audio";
    if (ext == ".gesc") return "scene";
    if (ext == ".js" || ext == ".mjs") return "script";
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl" ||
        ext == ".spv") {
        return "shader";
    }
    return "other";
}

struct ScanEntry {
    std::string absolute;   // 磁盘路径
    std::string logical;    // res:/ 形式（不在项目根下时退回绝对路径）
    std::string name;
    std::string ext;        // 小写，含点
    std::uintmax_t size = 0;
};

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

int GAsset_ScanDirectory(const char* dir, int recursive, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (!dir || !dir[0]) return -1;

    // 结尾斜杠先去掉：res:/models 与 res:/models/ 应当列出同一批条目，
    // 否则拼接出来的逻辑路径会出现双斜杠，界面上两条路径指向同一个文件。
    std::string logical_dir = dir;
    while (logical_dir.size() > 1 &&
           (logical_dir.back() == '/' || logical_dir.back() == '\\')) {
        logical_dir.pop_back();
    }

    const bool is_res = gryce_engine::resources::ResourcePath::is_resource_path(logical_dir);
    const std::string resolved =
        is_res ? gryce_engine::resources::ResourcePath::resolve(logical_dir) : logical_dir;

    std::error_code ec;
    if (!fs::is_directory(resolved, ec)) return -1;

    std::vector<ScanEntry> entries;
    auto collect = [&](const fs::path& p) {
        const std::string name = p.filename().string();
        if (is_meta_file(name)) return;   // 侧车不是资产

        ScanEntry e;
        e.name = name;
        e.ext = file_extension(name.c_str());
        std::error_code sec;
        const auto sz = fs::file_size(p, sec);
        e.size = sec ? 0 : static_cast<std::uintmax_t>(sz);

        if (is_res) {
            // 相对项目根的子路径，统一成正斜杠，与 res:/ 的书写风格一致。
            std::error_code rec;
            const fs::path rel = fs::relative(p, resolved, rec);
            std::string rel_str = rec ? name : rel.generic_string();
            e.logical = logical_dir + "/" + rel_str;
        } else {
            e.logical = p.generic_string();
        }
        entries.push_back(std::move(e));
    };

    if (recursive) {
        // 目录项顺序由文件系统决定；递归时先收集再整体排序，避免子目录
        // 之间的顺序随机。
        for (fs::recursive_directory_iterator it(resolved, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            std::error_code tec;
            if (!it->is_regular_file(tec) || tec) continue;
            collect(it->path());
        }
    } else {
        for (fs::directory_iterator it(resolved, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code tec;
            if (!it->is_regular_file(tec) || tec) continue;
            collect(it->path());
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const ScanEntry& a, const ScanEntry& b) { return a.logical < b.logical; });

    // items 先整体生成；header 里的 truncated 要在知道 buf_size 之后才能定，
    // 但 "0" 和 "1" 都是一字节，两版长度相同，因此长度可以提前算准。
    std::string items = "\"items\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ScanEntry& e = entries[i];
        if (i) items += ',';

        std::string guid;
        int has_meta = 0;
        int stale = 0;
        gryce_engine::assets::AssetMeta meta;
        if (gryce_engine::assets::read_asset_meta(e.absolute, meta)) {
            has_meta = 1;
            guid = meta.guid;
            stale = gryce_engine::assets::asset_meta_is_stale(e.absolute, meta) ? 1 : 0;
        }

        items += "{\"path\":\"";
        items += json_escape(e.logical);
        items += "\",\"name\":\"";
        items += json_escape(e.name);
        items += "\",\"ext\":\"";
        items += json_escape(e.ext);
        items += "\",\"size\":";
        items += std::to_string(e.size);
        items += ",\"guid\":\"";
        items += json_escape(guid);
        items += "\",\"has_meta\":";
        items += std::to_string(has_meta);
        items += ",\"stale\":";
        items += std::to_string(stale);
        items += ",\"kind\":\"";
        items += asset_kind(e.ext);
        items += "\"}";
    }
    items += ']';

    const std::string head =
        "{\"dir\":\"" + json_escape(logical_dir) + "\",\"count\":" +
        std::to_string(entries.size()) + ",\"truncated\":";
    const std::string tail = "}";

    const std::size_t full_len = head.size() + 1 /* truncated 一位 */ + 1 /* 逗号 */ +
                                 items.size() + tail.size();
    const int needed = static_cast<int>(full_len);

    if (!out_buf || buf_size <= 0) return needed;

    const bool fits = static_cast<std::size_t>(buf_size) > full_len;
    std::string json = head + (fits ? "0" : "1") + "," + items + tail;

    if (fits) {
        std::memcpy(out_buf, json.c_str(), json.size());
        out_buf[json.size()] = '\0';
        return static_cast<int>(json.size());
    }
    const std::size_t cap = static_cast<std::size_t>(buf_size) - 1;
    std::memcpy(out_buf, json.c_str(), cap);
    out_buf[cap] = '\0';
    return needed;
}

} // extern "C"
