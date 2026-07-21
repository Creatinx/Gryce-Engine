#include "gpack_bundle.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

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

} // namespace

GPackReader::~GPackReader() {
    if (file_) {
        std::fclose(file_);
    }
}

bool GPackReader::open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        GLOG_ERROR("GPackReader: failed to open '{}'", path);
        return false;
    }

    char magic[4] = {};
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "GPAK", 4) != 0) {
        GLOG_ERROR("GPackReader: invalid magic in '{}'", path);
        std::fclose(f);
        return false;
    }

    uint32_t version = 0;
    uint32_t count = 0;
    if (std::fread(&version, sizeof(version), 1, f) != 1 ||
        std::fread(&count, sizeof(count), 1, f) != 1) {
        GLOG_ERROR("GPackReader: failed to read header in '{}'", path);
        std::fclose(f);
        return false;
    }

    if (version != 1) {
        GLOG_ERROR("GPackReader: unsupported version {} in '{}'", version, path);
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
    }

    path_ = path;
    file_ = f;
    return true;
}

bool GPackReader::contains(const std::string& internal_path) const {
    for (const auto& e : entries_) {
        if (e.path == internal_path) return true;
    }
    return false;
}

std::vector<uint8_t> GPackReader::read(const std::string& internal_path) const {
    const GPackEntry* target = nullptr;
    for (const auto& e : entries_) {
        if (e.path == internal_path) {
            target = &e;
            break;
        }
    }
    if (!target || !file_) return {};

    if (std::fseek(file_, static_cast<long>(target->data_offset), SEEK_SET) != 0) {
        GLOG_ERROR("GPackReader: failed to seek '{}' in '{}'", internal_path, path_);
        return {};
    }

    std::vector<uint8_t> data(target->data_size);
    if (target->data_size > 0 &&
        std::fread(data.data(), 1, target->data_size, file_) != target->data_size) {
        GLOG_ERROR("GPackReader: failed to read '{}' from '{}'", internal_path, path_);
        return {};
    }
    return data;
}

bool GPackWriter::add_file(const std::string& internal_path, const std::string& source_path) {
    std::ifstream ifs(source_path, std::ios::binary);
    if (!ifs) {
        GLOG_ERROR("GPackWriter: failed to read source file '{}'", source_path);
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    buffers_.emplace_back(internal_path, std::move(data));
    return true;
}

bool GPackWriter::add_buffer(const std::string& internal_path, const std::vector<uint8_t>& data) {
    buffers_.emplace_back(internal_path, data);
    return true;
}

bool GPackWriter::write(const std::string& output_path) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(output_path).parent_path(), ec);

    FILE* out = std::fopen(output_path.c_str(), "wb");
    if (!out) {
        GLOG_ERROR("GPackWriter: failed to create '{}'", output_path);
        return false;
    }

    const uint32_t version = 1;
    const uint32_t count = static_cast<uint32_t>(buffers_.size());

    // Header: magic(4) + version(4) + count(4)
    std::fwrite("GPAK", 1, 4, out);
    std::fwrite(&version, sizeof(version), 1, out);
    std::fwrite(&count, sizeof(count), 1, out);

    // 计算每个 entry 头的大小，并预计算 data_offset
    std::vector<GPackEntry> entries;
    entries.reserve(count);
    uint64_t header_after_entries = 12;
    for (const auto& [path, data] : buffers_) {
        header_after_entries += static_cast<uint64_t>(sizeof(uint32_t) + path.size() +
                                                      sizeof(uint64_t) + sizeof(uint64_t) +
                                                      sizeof(uint32_t));
    }

    uint64_t current_offset = header_after_entries;
    for (const auto& [path, data] : buffers_) {
        GPackEntry entry;
        entry.path = path;
        entry.data_size = data.size();
        entry.data_offset = current_offset;
        entry.crc32 = compute_crc32(data.data(), data.size());
        entries.push_back(entry);
        current_offset += data.size();
    }

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

    const bool ok = std::ferror(out) == 0;
    std::fclose(out);
    if (ok) {
        GLOG_INFO("GPackWriter: wrote '{}' with {} entries", output_path, count);
    }
    return ok;
}

} // namespace gryce_engine::resources
