#include "vk_spirv_cache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_map>

namespace gryce_engine::render {

namespace {

// 进程内编译缓存：按 (stage + 完整源码) 作键，二次加载同名同内容 shader 时直接复用，
// 避免每帧/每对象重复编译。shader 变体由各自的 name 区分（如 pbr / skinned_pbr /
// gtao ...），故源码内容已足以唯一标识一个阶段的产物。
using SpirvCache = std::unordered_map<std::string, std::vector<uint32_t>>;
SpirvCache& spirv_cache() {
    static SpirvCache cache;
    return cache;
}

// 源码内容指纹（FNV-1a 64 位）。用内容而不是 mtime 判断缓存新鲜度：
// 复制/还原文件、编辑器保留 mtime 等操作都会让 mtime 失真，导致"改了 shader
// 却一直跑旧 SPIR-V"，这种问题极难排查，因此这里以内容为准。
std::string source_fingerprint(const std::string& source) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : source) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string hash_sidecar_path(const std::string& spv_path) { return spv_path + ".hash"; }

// 磁盘 SPIR-V 缓存命中判定：.hash 边车里记录的源码指纹与当前源码一致才算命中；
// 文件缺失/损坏/长度非 4 字节对齐一律视为未命中。
bool load_spirv_cache(const std::string& spv_path, const std::string& source,
                      std::vector<uint32_t>& out) {
    std::ifstream hash_file(hash_sidecar_path(spv_path));
    if (!hash_file.is_open()) return false;
    std::string recorded;
    std::getline(hash_file, recorded);
    if (recorded != source_fingerprint(source)) return false;

    std::ifstream file(spv_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0 || size % 4 != 0) return false;
    out.resize(static_cast<size_t>(size) / 4);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

void save_spirv_cache(const std::string& spv_path, const std::string& source,
                      const std::vector<uint32_t>& code) {
    if (code.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(spv_path).parent_path(), ec);
    if (ec) return;
    std::ofstream file(spv_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size()) * 4);
    file.close();
    std::ofstream hash_file(hash_sidecar_path(spv_path), std::ios::trunc);
    if (hash_file.is_open()) hash_file << source_fingerprint(source) << "\n";
}

std::string cache_key(GlslStage stage, const std::string& source) {
    return (stage == GlslStage::Vertex ? "vk_vert|" : "vk_frag|") + source;
}

} // namespace

bool load_or_compile_spirv(const std::string& source,
                           const std::string& file_name,
                           const std::string& spv_path,
                           GlslStage stage,
                           std::vector<uint32_t>& out,
                           std::string& error,
                           SpirvCacheSource* from) {
    if (from) *from = SpirvCacheSource::None;
    out.clear();
    if (source.empty()) {
        error = "empty shader source";
        return false;
    }

    // 1) 进程内缓存
    const std::string key = cache_key(stage, source);
    auto& cache = spirv_cache();
    if (auto it = cache.find(key); it != cache.end()) {
        out = it->second;
        if (from) *from = SpirvCacheSource::Memory;
        return true;
    }

    // 2) 磁盘持久化缓存（源码未改则连 shaderc 都不需要加载）
    if (!spv_path.empty() && load_spirv_cache(spv_path, source, out)) {
        cache.emplace(key, out);
        if (from) *from = SpirvCacheSource::Disk;
        return true;
    }

    // 3) shaderc 首编，成功后回填内存缓存并落盘
    if (!compile_glsl_to_spirv(source, file_name, stage, out, error)) {
        out.clear();
        return false;
    }
    if (!out.empty()) {
        cache.emplace(key, out);
        if (!spv_path.empty()) save_spirv_cache(spv_path, source, out);
    }
    return true;
}

} // namespace gryce_engine::render
