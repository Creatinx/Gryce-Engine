#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// GPackBundle — Gryce 资源包（.gpack）
//
// 二进制格式：
//   Header:
//     char     magic[4] = "GPAK"
//     uint32_t version  = 1
//     uint32_t entry_count
//   Entry (entry_count 次):
//     uint32_t path_len
//     char     path[path_len]
//     uint64_t data_size
//     uint64_t data_offset (文件起始偏移)
//     uint32_t crc32
//   Data: 各 entry 的二进制数据依次存放
//
// GPAK 用于打包一组资源（模型、纹理、场景等），便于热更新 / DLC 分发。
// ---------------------------------------------------------------------------

struct GPackEntry {
    std::string path;
    uint64_t data_size = 0;
    uint64_t data_offset = 0;
    uint32_t crc32 = 0;
};

class GPackReader {
public:
    GPackReader() = default;
    ~GPackReader();

    // 打开一个 .gpack 文件；失败返回 false
    bool open(const std::string& path);

    bool is_open() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }

    bool contains(const std::string& internal_path) const;

    // 读取内部路径对应的完整数据；找不到返回空 vector
    std::vector<uint8_t> read(const std::string& internal_path) const;

    const std::vector<GPackEntry>& entries() const { return entries_; }

private:
    std::string path_;
    mutable FILE* file_ = nullptr;
    std::vector<GPackEntry> entries_;
};

class GPackWriter {
public:
    // 添加一个文件到包中；internal_path 是包内路径（如 "models/cube.obj"）
    bool add_file(const std::string& internal_path, const std::string& source_path);

    // 添加内存数据到包中
    bool add_buffer(const std::string& internal_path, const std::vector<uint8_t>& data);

    // 写出 .gpack 文件
    bool write(const std::string& output_path) const;

    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& buffers() const {
        return buffers_;
    }

private:
    std::vector<std::pair<std::string, std::vector<uint8_t>>> buffers_;
};

} // namespace gryce_engine::resources
