#include "pak_bundle.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::resources {

namespace {

// 简单 CRC32 表
uint32_t crc32_table[256];
bool crc32_table_initialized = []() {
    for (int i = 0; i < 256; ++i) {
        uint32_t c = static_cast<uint32_t>(i);
        for (int j = 0; j < 8; ++j) {
            c = (c >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(c & 1u)));
        }
        crc32_table[i] = c;
    }
    return true;
}();

uint32_t compute_crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// Base64URL 编码（无填充）
// 将 src 的 src_len 字节编码为 Base64URL 字符串
std::string base64url_encode(const uint8_t* src, size_t src_len) {
    static const char kBase64Url[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((src_len + 2) / 3 * 4);
    for (size_t i = 0; i < src_len; i += 3) {
        const uint32_t b = (static_cast<uint32_t>(src[i]) << 16) |
                           (i + 1 < src_len ? static_cast<uint32_t>(src[i + 1]) << 8 : 0) |
                           (i + 2 < src_len ? static_cast<uint32_t>(src[i + 2]) : 0);
        out.push_back(kBase64Url[(b >> 18) & 0x3F]);
        out.push_back(kBase64Url[(b >> 12) & 0x3F]);
        out.push_back(i + 1 < src_len ? kBase64Url[(b >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < src_len ? kBase64Url[b & 0x3F] : '=');
    }
    // 移除填充字符 '='
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>

// 从 Windows CNG 获取随机字节
bool get_random_bytes(uint8_t* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
#else
// 从 /dev/urandom 获取随机字节
bool get_random_bytes(uint8_t* buf, size_t len) {
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t read = std::fread(buf, 1, len, f);
    std::fclose(f);
    return read == len;
}
#endif

} // namespace

// ============================================================================
// PakReader
// ============================================================================

PakReader::~PakReader() {
    if (file_) {
        std::fclose(file_);
    }
}

bool PakReader::open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        GLOG_ERROR("PakReader: failed to open '{}'", path);
        return false;
    }

    // 记录文件大小
    if (std::fseek(f, 0, SEEK_END) != 0) {
        GLOG_ERROR("PakReader: failed to seek end of '{}'", path);
        std::fclose(f);
        return false;
    }
    const long size_long = std::ftell(f);
    if (size_long < 0) {
        GLOG_ERROR("PakReader: failed to query size of '{}'", path);
        std::fclose(f);
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(size_long);
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    char magic[4] = {};
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "GPAK", 4) != 0) {
        GLOG_ERROR("PakReader: invalid magic in '{}'", path);
        std::fclose(f);
        return false;
    }

    uint32_t version = 0;
    uint32_t count = 0;
    uint64_t manifest_offset = 0;
    uint64_t manifest_size = 0;

    if (std::fread(&version, sizeof(version), 1, f) != 1 ||
        std::fread(&count, sizeof(count), 1, f) != 1) {
        GLOG_ERROR("PakReader: failed to read header in '{}'", path);
        std::fclose(f);
        return false;
    }

    if (version == 3) {
        // v3：包含 manifest 信息
        if (std::fread(&manifest_offset, sizeof(manifest_offset), 1, f) != 1 ||
            std::fread(&manifest_size, sizeof(manifest_size), 1, f) != 1) {
            GLOG_ERROR("PakReader: failed to read v3 header fields in '{}'", path);
            std::fclose(f);
            return false;
        }
    } else if (version != 1) {
        GLOG_ERROR("PakReader: unsupported version {} in '{}'", version, path);
        std::fclose(f);
        return false;
    }
    // version == 1: 标准 GPAK（无 manifest），version == 3: 带 manifest 的 GPAK

    // 每个 entry 头至少 24 字节
    const uint64_t header_min = (version == 3) ? 28 : 12;
    if (file_size < header_min || static_cast<uint64_t>(count) > (file_size - header_min) / 24u) {
        GLOG_ERROR("PakReader: entry count {} inconsistent with file size {} in '{}'",
                   count, file_size, path);
        std::fclose(f);
        return false;
    }

    entries_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t path_len = 0;
        if (std::fread(&path_len, sizeof(path_len), 1, f) != 1) {
            std::fclose(f);
            entries_.clear();
            return false;
        }
        if (static_cast<uint64_t>(path_len) > file_size) {
            GLOG_ERROR("PakReader: path_len {} exceeds file size in '{}'", path_len, path);
            std::fclose(f);
            entries_.clear();
            return false;
        }
        entries_[i].path.resize(path_len);
        if (path_len > 0 &&
            std::fread(entries_[i].path.data(), 1, path_len, f) != path_len) {
            std::fclose(f);
            entries_.clear();
            return false;
        }
        if (std::fread(&entries_[i].data_size, sizeof(entries_[i].data_size), 1, f) != 1 ||
            std::fread(&entries_[i].data_offset, sizeof(entries_[i].data_offset), 1, f) != 1 ||
            std::fread(&entries_[i].crc32, sizeof(entries_[i].crc32), 1, f) != 1) {
            std::fclose(f);
            entries_.clear();
            return false;
        }
        if (entries_[i].data_offset > file_size ||
            entries_[i].data_size > file_size - entries_[i].data_offset) {
            GLOG_ERROR("PakReader: entry '{}' data range exceeds file size in '{}'",
                       entries_[i].path, path);
            std::fclose(f);
            entries_.clear();
            return false;
        }
    }

    // 读取 manifest（v3）
    if (version == 3) {
        if (!read_manifest(f, manifest_offset, manifest_size)) {
            std::fclose(f);
            entries_.clear();
            return false;
        }
        // 构建查找表
        for (const auto& me : manifest_) {
            lookup_[me.original_path] = me.random_path;
        }
    }

    path_ = path;
    file_ = f;
    file_size_ = file_size;
    return true;
}

bool PakReader::read_manifest(FILE* f, uint64_t manifest_offset, uint64_t manifest_size) {
    if (manifest_offset > file_size_ || manifest_size > file_size_ - manifest_offset) {
        GLOG_ERROR("PakReader: manifest range exceeds file size in '{}'", path_);
        return false;
    }
    if (std::fseek(f, static_cast<long>(manifest_offset), SEEK_SET) != 0) {
        GLOG_ERROR("PakReader: failed to seek to manifest in '{}'", path_);
        return false;
    }

    uint32_t entry_count = 0;
    if (std::fread(&entry_count, sizeof(entry_count), 1, f) != 1) {
        GLOG_ERROR("PakReader: failed to read manifest entry count in '{}'", path_);
        return false;
    }

    manifest_.resize(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t orig_len = 0, random_len = 0;

        if (std::fread(&orig_len, sizeof(orig_len), 1, f) != 1) {
            GLOG_ERROR("PakReader: failed to read manifest orig_path_len in '{}'", path_);
            manifest_.clear();
            return false;
        }
        if (orig_len > 0) {
            manifest_[i].original_path.resize(orig_len);
            if (std::fread(manifest_[i].original_path.data(), 1, orig_len, f) != orig_len) {
                GLOG_ERROR("PakReader: failed to read manifest orig_path in '{}'", path_);
                manifest_.clear();
                return false;
            }
        }

        if (std::fread(&random_len, sizeof(random_len), 1, f) != 1) {
            GLOG_ERROR("PakReader: failed to read manifest random_path_len in '{}'", path_);
            manifest_.clear();
            return false;
        }
        if (random_len > 0) {
            manifest_[i].random_path.resize(random_len);
            if (std::fread(manifest_[i].random_path.data(), 1, random_len, f) != random_len) {
                GLOG_ERROR("PakReader: failed to read manifest random_path in '{}'", path_);
                manifest_.clear();
                return false;
            }
        }
    }

    return true;
}

bool PakReader::contains(const std::string& original_path) const {
    if (!lookup_.empty()) {
        return lookup_.find(original_path) != lookup_.end();
    }
    // v1 兼容：直接按 path 查找
    for (const auto& e : entries_) {
        if (e.path == original_path) return true;
    }
    return false;
}

std::string PakReader::resolve_random_path(const std::string& original_path) const {
    auto it = lookup_.find(original_path);
    if (it != lookup_.end()) return it->second;
    return {};
}

std::vector<uint8_t> PakReader::read(const std::string& original_path) const {
    // 通过 manifest 查找随机名称
    std::string target_path = original_path;
    if (!lookup_.empty()) {
        auto it = lookup_.find(original_path);
        if (it != lookup_.end()) {
            target_path = it->second;
        } else {
            return {};
        }
    }

    const PakEntry* target = nullptr;
    for (const auto& e : entries_) {
        if (e.path == target_path) {
            target = &e;
            break;
        }
    }
    if (!target || !file_) return {};

    std::lock_guard<std::mutex> lock(read_mutex_);

    if (target->data_offset > file_size_ || target->data_size > file_size_ - target->data_offset) {
        GLOG_ERROR("PakReader: '{}' data range exceeds file size in '{}'",
                   original_path, path_);
        return {};
    }

    if (std::fseek(file_, static_cast<long>(target->data_offset), SEEK_SET) != 0) {
        GLOG_ERROR("PakReader: failed to seek '{}' in '{}'", original_path, path_);
        return {};
    }

    std::vector<uint8_t> data(target->data_size);
    if (target->data_size > 0 &&
        std::fread(data.data(), 1, target->data_size, file_) != target->data_size) {
        GLOG_ERROR("PakReader: failed to read '{}' from '{}'", original_path, path_);
        return {};
    }
    return data;
}

// ============================================================================
// PakWriter
// ============================================================================

std::string PakWriter::generate_random_name() {
    // 32 字节随机数 -> Base64URL 编码（43 字符，无填充）
    uint8_t random_bytes[32];
    if (!get_random_bytes(random_bytes, sizeof(random_bytes))) {
        // 失败时回退到时间种子 + 计数器
        static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0; i < sizeof(random_bytes); i += 8) {
            uint64_t v = rng();
            std::memcpy(random_bytes + i, &v, (sizeof(random_bytes) - i < 8) ? (sizeof(random_bytes) - i) : 8);
        }
    }
    return base64url_encode(random_bytes, sizeof(random_bytes));
}

bool PakWriter::add_file(const std::string& original_path, const std::string& source_path) {
    std::ifstream ifs(source_path, std::ios::binary);
    if (!ifs) {
        GLOG_ERROR("PakWriter: failed to read source file '{}'", source_path);
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());

    const std::string random_name = generate_random_name();
    buffers_.emplace_back(random_name, std::move(data));
    manifest_.push_back({original_path, random_name});
    return true;
}

bool PakWriter::add_buffer(const std::string& original_path, const std::vector<uint8_t>& data) {
    const std::string random_name = generate_random_name();
    buffers_.emplace_back(random_name, data);
    manifest_.push_back({original_path, random_name});
    return true;
}

bool PakWriter::write(const std::string& output_path) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(output_path).parent_path(), ec);

    FILE* out = std::fopen(output_path.c_str(), "wb");
    if (!out) {
        GLOG_ERROR("PakWriter: failed to create '{}'", output_path);
        return false;
    }

    const uint32_t version = 3;
    const uint32_t count = static_cast<uint32_t>(buffers_.size());
    const uint32_t manifest_count = static_cast<uint32_t>(manifest_.size());

    // 计算 entry 表大小，预计算 data_offset
    uint64_t header_after_entries = 28; // magic(4) + version(4) + count(4) + manifest_offset(8) + manifest_size(8)
    for (const auto& [path, data] : buffers_) {
        header_after_entries += static_cast<uint64_t>(sizeof(uint32_t) + path.size() +
                                                      sizeof(uint64_t) + sizeof(uint64_t) +
                                                      sizeof(uint32_t));
    }

    // 计算 manifest 大小
    uint64_t manifest_size = sizeof(uint32_t); // entry_count
    for (const auto& me : manifest_) {
        manifest_size += sizeof(uint32_t) + me.original_path.size() +
                         sizeof(uint32_t) + me.random_path.size();
    }

    // 布局：header + entries + data + manifest
    // 构建 entries 表
    std::vector<PakEntry> entries;
    entries.reserve(count);
    uint64_t current_offset = header_after_entries;
    for (const auto& [path, data] : buffers_) {
        PakEntry entry;
        entry.path = path;
        entry.data_size = data.size();
        entry.data_offset = current_offset;
        entry.crc32 = compute_crc32(data.data(), data.size());
        entries.push_back(entry);
        current_offset += data.size();
    }

    // manifest 在 data 之后
    const uint64_t final_manifest_offset = current_offset;

    // 写入 header
    std::fwrite("GPAK", 1, 4, out);
    std::fwrite(&version, sizeof(version), 1, out);
    std::fwrite(&count, sizeof(count), 1, out);
    std::fwrite(&final_manifest_offset, sizeof(final_manifest_offset), 1, out);
    std::fwrite(&manifest_size, sizeof(manifest_size), 1, out);

    // 写入 entry 表
    for (const auto& e : entries) {
        uint32_t path_len = static_cast<uint32_t>(e.path.size());
        std::fwrite(&path_len, sizeof(path_len), 1, out);
        if (path_len > 0) {
            std::fwrite(e.path.data(), 1, path_len, out);
        }
        std::fwrite(&e.data_size, sizeof(e.data_size), 1, out);
        std::fwrite(&e.data_offset, sizeof(e.data_offset), 1, out);
        std::fwrite(&e.crc32, sizeof(e.crc32), 1, out);
    }

    // 写入数据
    for (size_t i = 0; i < buffers_.size(); ++i) {
        const auto& data = buffers_[i].second;
        if (!data.empty()) {
            std::fwrite(data.data(), 1, data.size(), out);
        }
    }

    // 写入 manifest
    std::fwrite(&manifest_count, sizeof(manifest_count), 1, out);
    for (const auto& me : manifest_) {
        uint32_t orig_len = static_cast<uint32_t>(me.original_path.size());
        uint32_t random_len = static_cast<uint32_t>(me.random_path.size());
        std::fwrite(&orig_len, sizeof(orig_len), 1, out);
        if (orig_len > 0) {
            std::fwrite(me.original_path.data(), 1, orig_len, out);
        }
        std::fwrite(&random_len, sizeof(random_len), 1, out);
        if (random_len > 0) {
            std::fwrite(me.random_path.data(), 1, random_len, out);
        }
    }

    const bool ok = std::ferror(out) == 0;
    std::fclose(out);
    if (ok) {
        GLOG_INFO("PakWriter: wrote '{}' with {} entries (v3, manifest at offset {})",
                  output_path, count, final_manifest_offset);
    }
    return ok;
}

} // namespace gryce_engine::resources