// ===========================================================================
// editor/ext/asset/asset_index.cpp — 资产浏览实现
// ===========================================================================

#include "asset/asset_index.h"

#include "common/json_writer.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace gryce::editor::asset {

namespace {

constexpr int kInitialBuffer = 8 * 1024;
// 面板每帧都会问一次，缓存窗口取 200ms：既不会读到明显过期的内容，
// 也能把"60 次/秒扫盘"压到"5 次/秒"。
constexpr double kCacheTtlMs = 200.0;

struct CacheEntry {
    std::string json;
    double stamp_ms = 0.0;
};

std::map<std::string, CacheEntry> g_cache;

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(
               steady_clock::now().time_since_epoch()).count();
}

std::string empty_result(const char* dir) {
    Json j;
    j.begin_object();
    j.kv("dir", dir ? dir : "");
    j.kv("count", 0);
    j.kv("truncated", 0);
    j.key("items");
    j.begin_array();
    j.end_array();
    j.end_object();
    return j.str();
}

// 引擎按"返回所需长度"约定：一次给不够就按需扩一次。理论上扩一次就够
// （返回的是精确长度），但并发目录变动可能让它继续变大，所以留两轮。
std::string call_scan(const char* dir, int recursive) {
    int cap = kInitialBuffer;
    std::vector<char> buf(static_cast<std::size_t>(cap));

    for (int attempt = 0; attempt < 3; ++attempt) {
        const int n = GAsset_ScanDirectory(dir, recursive, buf.data(), cap);
        if (n < 0) return empty_result(dir);
        if (n < cap) return std::string(buf.data(), static_cast<std::size_t>(n));
        cap = n + 1;
        buf.assign(static_cast<std::size_t>(cap), '\0');
    }
    return empty_result(dir);
}

} // namespace

void reset() { g_cache.clear(); }

const char* scan(const char* dir, int recursive) {
    const std::string key = std::string(dir ? dir : "") + (recursive ? "|r" : "|f");

    auto it = g_cache.find(key);
    if (it != g_cache.end() && now_ms() - it->second.stamp_ms < kCacheTtlMs) {
        return it->second.json.c_str();
    }

    CacheEntry entry;
    entry.json = call_scan(dir, recursive);
    entry.stamp_ms = now_ms();

    auto ins = g_cache.insert_or_assign(key, std::move(entry));
    // map 的节点地址稳定，value 的 c_str 在下次更新这个键之前一直有效。
    return ins.first->second.json.c_str();
}

GAssetHandle import(const char* path) {
    if (!path || !path[0]) return -1;
    const GAssetHandle h = GAsset_Import(path);
    // 新资产会多出侧车文件，列表和元信息都可能变。
    invalidate();
    return h;
}

std::string meta_json(const char* path) {
    Json j;
    j.begin_object();
    if (!path || !path[0]) {
        j.kv("ok", 0);
        j.end_object();
        return j.str();
    }

    GAssetMetaInfo info{};
    if (!GAsset_ReadMeta(path, &info)) {
        j.kv("ok", 0);
        j.kv("path", path);
        j.end_object();
        return j.str();
    }

    j.kv("ok", 1);
    j.kv("path", path);
    j.kv("guid", info.guid);
    j.kv("importer", info.importer);
    j.kv("version", info.version);
    j.kv("scale", info.scale);
    j.kv("upAxis", info.up_axis);
    j.kv("generateLod", info.generate_lod);
    j.kv("importMaterials", info.import_materials);
    j.kv("roughnessScale", info.roughness_scale);
    j.kv("stale", GAsset_IsMetaStale(path));
    j.end_object();
    return j.str();
}

void invalidate() { g_cache.clear(); }

} // namespace gryce::editor::asset
