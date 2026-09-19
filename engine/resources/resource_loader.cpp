#include "resource_loader.h"

#include <cstring>

#include "aes_gcm.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::resources {

namespace {

// 主密钥：两段拆分常量异或组合（仅防普通用户直接读取明文）
void derive_master_key(uint8_t out[32]) {
    static const uint8_t kPartA[32] = {
        0x8d, 0x1f, 0xa3, 0x54, 0x77, 0x0c, 0xe9, 0x2b,
        0x5a, 0x91, 0x3d, 0x6c, 0x04, 0xb8, 0x72, 0xce,
        0x19, 0xe0, 0x4f, 0x85, 0x66, 0x2d, 0x9a, 0xb1,
        0x7c, 0x43, 0xf6, 0x08, 0xac, 0x59, 0x2e, 0xd4};
    static const uint8_t kPartB[32] = {
        0x2f, 0x79, 0xd8, 0x1a, 0x6c, 0x51, 0x80, 0x9f,
        0x3e, 0xa7, 0x4b, 0x12, 0x75, 0xcd, 0x08, 0xb6,
        0x47, 0x9c, 0x63, 0xfa, 0x21, 0x0e, 0x84, 0x5d,
        0xeb, 0x30, 0x99, 0x6f, 0xd1, 0x2a, 0x77, 0x8c};
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<uint8_t>(kPartA[i] ^ kPartB[i]);
    }
}

// 读取加密块并解密；失败返回空
std::vector<uint8_t> decrypt_block(const std::vector<uint8_t>& cipher,
                                   const char* what) {
    uint8_t key[32];
    derive_master_key(key);
    std::vector<uint8_t> plain;
    if (!aes_gcm_decrypt_buf(key, nullptr, 0, cipher.data(), cipher.size(), plain)) {
        GLOG_ERROR("ResourceLoader: GCM tag verification failed for {}", what);
        return {};
    }
    return plain;
}

// 校验并剥离 4 字节魔数；魔数不符返回空
bool check_magic(std::vector<uint8_t>& payload, const char magic[4], const char* what) {
    if (payload.size() < 4 ||
        std::memcmp(payload.data(), magic, 4) != 0) {
        GLOG_ERROR("ResourceLoader: chunk magic mismatch for {}", what);
        payload.clear();
        return false;
    }
    payload.erase(payload.begin(), payload.begin() + 4);
    return true;
}

} // namespace

void resource_master_key(uint8_t out[32]) {
    derive_master_key(out);
}

// ============================================================================
// ResourceLoader
// ============================================================================

bool ResourceLoader::mount(const std::string& pak_path) {
    return reader_.open(pak_path);
}

std::vector<uint8_t> ResourceLoader::read_raw(const std::string& logical_path) const {
    return reader_.read(logical_path);
}

std::vector<uint8_t> ResourceLoader::decrypt_chunk(const std::string& logical_path) const {
    const std::vector<uint8_t> cipher = reader_.read(logical_path);
    if (cipher.empty()) {
        GLOG_ERROR("ResourceLoader: '{}' not found in '{}'", logical_path, path());
        return {};
    }
    return decrypt_block(cipher, logical_path.c_str());
}

std::string ResourceLoader::decrypt_ui_text(const std::string& logical_path) const {
    std::vector<uint8_t> plain = decrypt_chunk(logical_path);
    if (!check_magic(plain, kChunkMagicUiText, logical_path.c_str())) {
        return {};
    }
    return std::string(plain.begin(), plain.end());
}

std::vector<uint8_t> ResourceLoader::decrypt_js_bytecode(const std::string& logical_path) const {
    std::vector<uint8_t> plain = decrypt_chunk(logical_path);
    if (!check_magic(plain, kChunkMagicJsBytecode, logical_path.c_str())) {
        return {};
    }
    return plain;
}

// ============================================================================
// 打包辅助
// ============================================================================

std::vector<uint8_t> ResourceLoader::pack_js_bytecode(const std::vector<uint8_t>& bytecode) {
    std::vector<uint8_t> payload;
    payload.reserve(4 + bytecode.size());
    payload.insert(payload.end(), kChunkMagicJsBytecode, kChunkMagicJsBytecode + 4);
    payload.insert(payload.end(), bytecode.begin(), bytecode.end());

    uint8_t key[32];
    derive_master_key(key);
    std::vector<uint8_t> out;
    if (!aes_gcm_encrypt_buf(key, nullptr, 0, payload.data(), payload.size(), out)) {
        GLOG_ERROR("ResourceLoader: pack_js_bytecode encryption failed");
        return {};
    }
    return out;
}

std::vector<uint8_t> ResourceLoader::pack_ui_text(const std::string& text) {
    std::vector<uint8_t> payload;
    payload.reserve(4 + text.size());
    payload.insert(payload.end(), kChunkMagicUiText, kChunkMagicUiText + 4);
    payload.insert(payload.end(), text.begin(), text.end());

    uint8_t key[32];
    derive_master_key(key);
    std::vector<uint8_t> out;
    if (!aes_gcm_encrypt_buf(key, nullptr, 0, payload.data(), payload.size(), out)) {
        GLOG_ERROR("ResourceLoader: pack_ui_text encryption failed");
        return {};
    }
    return out;
}

std::vector<uint8_t> ResourceLoader::pack_raw(const std::vector<uint8_t>& data) {
    return data;
}

} // namespace gryce_engine::resources
