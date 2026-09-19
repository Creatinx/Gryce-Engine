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

// ---------------------------------------------------------------------------
// ChaCha20（RFC 7539）流加密，用于 GPAK v4 数据区加密。
// 32 字节密钥 + 12 字节 nonce + 32 位块计数器（counter 从 byte_offset/64 起）。
// chacha20_xor 支持从任意字节偏移对缓冲区加/解密（加密与解密为同一互逆操作）。
// 纯 C++ 实现，不依赖平台加密库，确保 Linux 构建一致。
// ---------------------------------------------------------------------------

static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void chacha20_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d = rotl32(d ^ a, 16);
    c += d; b = rotl32(b ^ c, 12);
    a += b; d = rotl32(d ^ a, 8);
    c += d; b = rotl32(b ^ c, 7);
}

// 生成单个 64 字节 keystream 块。counter 独立于 nonce（计数器低 32 位拼接 nonce）。
static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    static const uint32_t kConst[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    uint32_t state[16];
    for (int i = 0; i < 4; ++i) state[i] = kConst[i];
    for (int i = 0; i < 8; ++i)
        state[4 + i] = (static_cast<uint32_t>(key[i * 4]) << 24) |
                       (static_cast<uint32_t>(key[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(key[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(key[i * 4 + 3]);
    state[12] = counter;
    for (int i = 0; i < 3; ++i)
        state[13 + i] = (static_cast<uint32_t>(nonce[i * 4]) << 24) |
                        (static_cast<uint32_t>(nonce[i * 4 + 1]) << 16) |
                        (static_cast<uint32_t>(nonce[i * 4 + 2]) << 8) |
                        static_cast<uint32_t>(nonce[i * 4 + 3]);

    uint32_t ws[16];
    std::memcpy(ws, state, sizeof(ws));
    for (int i = 0; i < 10; ++i) {
        chacha20_quarter_round(ws[0], ws[4],  ws[8], ws[12]);
        chacha20_quarter_round(ws[1], ws[5],  ws[9], ws[13]);
        chacha20_quarter_round(ws[2], ws[6], ws[10], ws[14]);
        chacha20_quarter_round(ws[3], ws[7], ws[11], ws[15]);
        chacha20_quarter_round(ws[0], ws[5], ws[10], ws[15]);
        chacha20_quarter_round(ws[1], ws[6], ws[11], ws[12]);
        chacha20_quarter_round(ws[2], ws[7], ws[ 8], ws[13]);
        chacha20_quarter_round(ws[3], ws[4], ws[ 9], ws[14]);
    }
    for (int i = 0; i < 16; ++i) ws[i] += state[i];

    uint8_t* b = out;
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = ws[i];
        b[i * 4]     = static_cast<uint8_t>(v);
        b[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
        b[i * 4 + 2] = static_cast<uint8_t>(v >> 16);
        b[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
    }
}

// 对 buf 的 len 个字节，用与 byte_offset 处 keystream 对齐的流做 XOR。
// 加密与解密都调用本函数（XOR 互逆）。
static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                         uint64_t byte_offset, uint8_t* buf, size_t len) {
    while (len > 0) {
        const uint64_t block_index = byte_offset / 64;
        const size_t start_in_block = static_cast<size_t>(byte_offset % 64);
        uint8_t ks[64];
        chacha20_block(key, static_cast<uint32_t>(block_index), nonce, ks);
        const size_t take = (len < (64 - start_in_block)) ? len : (64 - start_in_block);
        for (size_t i = 0; i < take; ++i) buf[i] ^= ks[start_in_block + i];
        buf += take;
        len -= take;
        byte_offset += take;
    }
}

// 全局加解密密钥（32 字节原始）。PakWriter::write 与 PakReader::read 共享。
// 长度为 32 时才启用加密；否则不加密。
std::string& pak_crypto_key() {
    static std::string key;
    return key;
}

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
    // 尽早记录路径与文件大小，供后续 manifest 校验与读取使用
    path_ = path;
    file_size_ = file_size;

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

    if (version == 3 || version == 4) {
        // v3/v4：包含 manifest 信息
        if (std::fread(&manifest_offset, sizeof(manifest_offset), 1, f) != 1 ||
            std::fread(&manifest_size, sizeof(manifest_size), 1, f) != 1) {
            GLOG_ERROR("PakReader: failed to read v3/v4 header fields in '{}'", path);
            std::fclose(f);
            return false;
        }
        if (version == 4) {
            // v4：多一个 flags 字节；bit0=1 表示 data 区以 ChaCha20 加密，后随 12 字节 nonce
            uint8_t flags = 0;
            if (std::fread(&flags, sizeof(flags), 1, f) != 1) {
                GLOG_ERROR("PakReader: failed to read v4 flags in '{}'", path);
                std::fclose(f);
                return false;
            }
            encrypted_ = (flags & 0x01u) ? 1u : 0u;
            if (encrypted_) {
                nonce_.resize(12);
                if (std::fread(nonce_.data(), 1, nonce_.size(), f) != nonce_.size()) {
                    GLOG_ERROR("PakReader: failed to read v4 nonce in '{}'", path);
                    std::fclose(f);
                    return false;
                }
            }
        }
    } else if (version != 1) {
        GLOG_ERROR("PakReader: unsupported version {} in '{}'", version, path);
        std::fclose(f);
        return false;
    }
    // version == 1: 标准 GPAK（无 manifest），version == 3: 带 manifest 的 GPAK，
    // version == 4: 带 manifest + 可选数据区加密的 GPAK

    // 每个 entry 头至少 24 字节
    const uint64_t header_min = (version == 3 || version == 4) ? 28 : 12;
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

    // 读取 manifest（v3/v4）
    if (version == 3 || version == 4) {
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
    // v4 加密包：用全局密钥 + 本包 nonce，按 data_offset 处对齐的流解密
    if (encrypted_ && target->data_size > 0) {
        const std::string& key = pak_crypto_key();
        if (key.size() == 32 && nonce_.size() == 12) {
            chacha20_xor(reinterpret_cast<const uint8_t*>(key.data()), nonce_.data(),
                         target->data_offset, data.data(), data.size());
        } else {
            GLOG_ERROR("PakReader: '{}' is encrypted but no decrypt key is set in '{}'",
                       original_path, path_);
            return {};
        }
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

    // 加密开关：全局密钥长度为 32 字节时启用 ChaCha20 数据区加密，写出 GPAK v4
    const std::string& key = pak_crypto_key();
    const bool encrypt = (key.size() == 32);
    const uint32_t version = encrypt ? 4 : 3;
    uint8_t nonce[12] = {};
    if (encrypt) {
        if (!get_random_bytes(nonce, sizeof(nonce))) {
            GLOG_ERROR("PakWriter: failed to generate encryption nonce");
            std::fclose(out);
            return false;
        }
    }
    const uint32_t count = static_cast<uint32_t>(buffers_.size());
    const uint32_t manifest_count = static_cast<uint32_t>(manifest_.size());

    // head_overhead：v4 额外 flags(1) + nonce(12)；v3 无
    const uint64_t head_overhead = encrypt ? (1 + 12) : 0;
    // 计算 entry 表大小，预计算 data_offset
    uint64_t header_after_entries = 28 + head_overhead; // magic(4) + version(4) + count(4) + manifest_offset(8) + manifest_size(8)
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
    if (encrypt) {
        const uint8_t flags = 0x01u;  // bit0 = 数据区加密
        std::fwrite(&flags, sizeof(flags), 1, out);
        std::fwrite(nonce, 1, sizeof(nonce), out);
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
        if (data.empty()) continue;
        if (encrypt) {
            // 用本包 nonce + 该 entry 的 data_offset 对齐的流加密后写出
            std::vector<uint8_t> enc = data;
            chacha20_xor(reinterpret_cast<const uint8_t*>(key.data()), nonce,
                         entries[i].data_offset, enc.data(), enc.size());
            std::fwrite(enc.data(), 1, enc.size(), out);
        } else {
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
        GLOG_INFO("PakWriter: wrote '{}' with {} entries (v{}, {}data, manifest at offset {})",
                  output_path, count, version,
                  encrypt ? "encrypted " : "",
                  final_manifest_offset);
    }
    return ok;
}

void set_pak_crypto_key(const std::string& new_key) {
    pak_crypto_key() = new_key;
}

} // namespace gryce_engine::resources