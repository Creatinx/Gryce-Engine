// asset_meta.cpp — 资产侧车文件（<asset>.meta）的读写、GUID 生成与过期判定。
// 侧车文件与源资产放在同一目录，由 AssetManager 在首次导入时自动创建；
// 编辑器通过 asset_api 的 GAsset_*Meta 系列接口读写它。
#include "assets/asset_meta.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

namespace fs = std::filesystem;

// GUID 生成器与它自己的锁：与 AssetManager 的 mutex_ 无关，可被任意线程调用。
std::mutex g_guid_mutex;
std::mt19937_64 g_guid_engine = [] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(),
                      static_cast<unsigned>(
                          std::chrono::steady_clock::now().time_since_epoch().count())};
    return std::mt19937_64(seq);
}();

// 源文件指纹：最后写入时间（纳秒）+ 文件大小。
// 用 file_clock 的原始计数而不是转换到 system_clock —— 侧车只在同一台机器上读写，
// 不跨机器比较，省掉一次易错的纪元换算。
struct SourceStamp {
    bool valid = false;
    std::int64_t mtime_ns = 0;
    std::int64_t size = 0;
};

SourceStamp read_source_stamp(const std::string& asset_path) {
    SourceStamp stamp;
    std::error_code ec;
    if (asset_path.empty()) return stamp;
    const fs::path p(asset_path);
    if (!fs::exists(p, ec) || ec) return stamp;
    const auto mtime = fs::last_write_time(p, ec);
    if (ec) return stamp;
    stamp.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         mtime.time_since_epoch())
                         .count();
    const auto size = fs::file_size(p, ec);
    stamp.size = ec ? 0 : static_cast<std::int64_t>(size);
    stamp.valid = true;
    return stamp;
}

} // namespace

std::string asset_meta_path(const std::string& asset_path) {
    if (asset_path.empty()) return std::string();
    return asset_path + ".meta";
}

std::string generate_asset_guid() {
    // 2 x 64 位随机数拼成 32 位十六进制。random_device 在 MSYS2/MinGW 上走系统
    // 熵源；再混入 steady_clock 计数，避免极端情况下随机池重复。
    std::lock_guard<std::mutex> lock(g_guid_mutex);
    const std::uint64_t a = g_guid_engine();
    const std::uint64_t b = g_guid_engine();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return std::string(buf, 32);
}

bool asset_meta_validate(const AssetMeta& meta) {
    if (meta.scale <= 0.0f || !std::isfinite(meta.scale)) return false;
    if (meta.up_axis < 0 || meta.up_axis > 2) return false;
    if (meta.roughness_scale <= 0.0f || !std::isfinite(meta.roughness_scale)) return false;
    return true;
}

bool read_asset_meta(const std::string& asset_path, AssetMeta& out) {
    const std::string meta_path = asset_meta_path(asset_path);
    if (meta_path.empty()) return false;

    std::error_code ec;
    if (!fs::exists(meta_path, ec) || ec) return false;

    std::ifstream in(meta_path, std::ios::binary);
    if (!in) return false;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        GLOG_WARN("asset_meta: {} 解析失败（按缺失处理）: {}", meta_path, e.what());
        return false;
    }
    if (!j.is_object()) return false;

    AssetMeta meta;
    // 缺失字段一律保留默认值：侧车可能由旧版本写出，也允许手写精简版。
    meta.guid = j.value("guid", std::string());
    meta.source_path = j.value("source_path", std::string());
    meta.importer = j.value("importer", std::string());
    meta.scale = j.value("scale", 1.0f);
    meta.up_axis = j.value("up_axis", 1);
    meta.generate_lod = j.value("generate_lod", true);
    meta.import_materials = j.value("import_materials", true);
    meta.roughness_scale = j.value("roughness_scale", 1.0f);
    meta.source_mtime_ns = j.value("source_mtime_ns", static_cast<std::int64_t>(0));
    meta.source_size = j.value("source_size", static_cast<std::int64_t>(0));
    meta.version = j.value("version", 1);

    if (meta.version > kAssetMetaVersion) {
        GLOG_WARN("asset_meta: {} 版本 {} 高于引擎支持的 {}，按兼容方式读取",
                  meta_path, meta.version, kAssetMetaVersion);
    }
    if (meta.guid.empty()) {
        // 手工放的 / 老版本侧车可能没有 GUID，补一个并立即写回，
        // 否则每次导入都会生成不同标识，引用无法稳定。
        meta.guid = generate_asset_guid();
        GLOG_INFO("asset_meta: {} 缺少 guid，已生成 {}", meta_path, meta.guid);
    }
    if (!asset_meta_validate(meta)) {
        GLOG_WARN("asset_meta: {} 导入设置非法，回退默认值", meta_path);
        const std::string keep_guid = meta.guid;
        meta = AssetMeta{};
        meta.guid = keep_guid;
    }
    out = std::move(meta);
    return true;
}

bool write_asset_meta(const std::string& asset_path, const AssetMeta& meta) {
    const std::string meta_path = asset_meta_path(asset_path);
    if (meta_path.empty()) return false;

    nlohmann::json j;
    j["version"] = kAssetMetaVersion;
    j["guid"] = meta.guid;
    j["source_path"] = meta.source_path;
    j["importer"] = meta.importer;
    j["scale"] = meta.scale;
    j["up_axis"] = meta.up_axis;
    j["generate_lod"] = meta.generate_lod;
    j["import_materials"] = meta.import_materials;
    j["roughness_scale"] = meta.roughness_scale;
    j["source_mtime_ns"] = meta.source_mtime_ns;
    j["source_size"] = meta.source_size;

    std::error_code ec;
    const fs::path parent = fs::path(meta_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);   // 已存在时 ec 保持清零
    }

    std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        // 只读目录 / 资源包内资产：属于正常情况，降级为警告而不是错误。
        GLOG_WARN("asset_meta: 无法写入 {}（目录只读？）", meta_path);
        return false;
    }
    out << j.dump(2) << '\n';
    return out.good();
}

bool load_or_create_asset_meta(const std::string& asset_path, AssetMeta& out) {
    if (read_asset_meta(asset_path, out)) return true;

    AssetMeta meta;
    meta.guid = generate_asset_guid();
    meta.source_path = asset_path;
    meta.importer = "mesh";   // 由调用方按扩展名覆盖
    const SourceStamp stamp = read_source_stamp(asset_path);
    meta.source_mtime_ns = stamp.mtime_ns;
    meta.source_size = stamp.size;
    out = meta;
    // 源文件不存在（例如资源包内资产）时不写盘：没有意义的侧车会污染工程目录。
    if (stamp.valid) {
        write_asset_meta(asset_path, meta);
    }
    return true;
}

bool asset_meta_is_stale(const std::string& asset_path, const AssetMeta& meta) {
    // 没记录过指纹（手工精简侧车）视为不过期：宁可少提示，也不要每次都判脏。
    if (meta.source_mtime_ns == 0 && meta.source_size == 0) return false;
    const SourceStamp stamp = read_source_stamp(asset_path);
    if (!stamp.valid) return false;
    return stamp.mtime_ns != meta.source_mtime_ns || stamp.size != meta.source_size;
}

} // namespace gryce_engine::assets
