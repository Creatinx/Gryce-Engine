#pragma once

#include "../export.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// PakBundle — Gryce 资源包（.pak）
//
// 二进制格式（GPAK v3）：
//   Header:
//     char     magic[4] = "GPAK"
//     uint32_t version  = 3
//     uint32_t entry_count
//     uint64_t manifest_offset   // 文件起始到 manifest 的偏移
//     uint64_t manifest_size     // manifest 区域大小
//   Entry (entry_count 次):
//     uint32_t path_len
//     char     path[path_len]            // 随机 Base64 名称
//     uint64_t data_size
//     uint64_t data_offset               // 文件起始偏移
//     uint32_t crc32
//   Data: 各 entry 的二进制数据依次存放
//   Manifest (manifest_offset 偏移处):
//     uint32_t entry_count
//     ManifestEntry (entry_count 次):
//       uint32_t orig_path_len
//       char     orig_path[orig_path_len]    // 原始路径，如 "models/cube.obj"
//       uint32_t random_path_len
//       char     random_path[random_path_len]  // 随机 Base64 名称
//
// 资源文件的包内路径是随机 Base64 字符串，无逻辑含义。
// 通过 manifest 表将原始路径（如 "models/cube.obj"）映射到随机名称。
// 运行时通过原始路径查找资源，PakReader 自动完成映射。
// ---------------------------------------------------------------------------

struct PakEntry {
    std::string path;       // 随机 Base64 名称
    uint64_t data_size = 0;
    uint64_t data_offset = 0;
    uint32_t crc32 = 0;
};

struct PakManifestEntry {
    std::string original_path;   // 原始路径，如 "models/cube.obj"
    std::string random_path;     // 随机 Base64 名称
};

class GRYCE_API PakReader {
public:
    PakReader() = default;
    ~PakReader();

    // 打开一个 .pak 文件；失败返回 false
    bool open(const std::string& path);

    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }

    // 按原始路径查询是否存在
    bool contains(const std::string& original_path) const;

    // 读取原始路径对应的完整数据；找不到返回空 vector
    std::vector<uint8_t> read(const std::string& original_path) const;

    const std::vector<PakEntry>& entries() const { return entries_; }
    const std::vector<PakManifestEntry>& manifest() const { return manifest_; }

    // 通过原始路径查找随机名称
    std::string resolve_random_path(const std::string& original_path) const;

private:
    bool read_manifest(FILE* f, uint64_t manifest_offset, uint64_t manifest_size);

    std::string path_;
    mutable FILE* file_ = nullptr;
    std::vector<PakEntry> entries_;
    std::vector<PakManifestEntry> manifest_;
    // 原始路径 -> 随机名称 快速查找
    mutable std::unordered_map<std::string, std::string> lookup_;
    uint64_t file_size_ = 0;
    mutable std::mutex read_mutex_;
};

class GRYCE_API PakWriter {
public:
    // 添加一个文件到包中；original_path 是原始路径（如 "models/cube.obj"）
    // 会自动生成随机 Base64 名称
    bool add_file(const std::string& original_path, const std::string& source_path);

    // 添加内存数据到包中
    bool add_buffer(const std::string& original_path, const std::vector<uint8_t>& data);

    // 写出 .pak 文件
    bool write(const std::string& output_path) const;

    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& buffers() const {
        return buffers_;
    }

    const std::vector<PakManifestEntry>& manifest() const { return manifest_; }

private:
    // 生成随机 Base64 名称（32字节随机数 -> Base64URL 编码，43字符，无填充）
    static std::string generate_random_name();

    // 原始路径 -> 数据
    std::vector<std::pair<std::string, std::vector<uint8_t>>> buffers_;
    // Manifest 条目
    std::vector<PakManifestEntry> manifest_;
};

} // namespace gryce_engine::resources