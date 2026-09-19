#pragma once

// AES-256-GCM 加解密（纯 C++ 实现，跨平台，Windows / Linux 行为一致）
//
// 用途：发布打包阶段对 JS 字节码与 DSL 文本进行加密。
// 不依赖 OpenSSL / CNG，便于 build.py 在 Linux 上直接编译。
// 仅用于防止普通用户直接读取明文，非真正的安全边界。
//
// 便捷缓冲格式（encrypt_buf / decrypt_buf）：
//   iv(12) || ciphertext(len) || tag(16)
// 其中 iv 为每块随机生成的 96 位初始向量，tag 为 16 字节 GCM 认证标签。

#include "../export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// 底层接口
// ---------------------------------------------------------------------------

// 加密。ciphertext 长度与 plaintext 相同；tag 输出 16 字节认证标签。
// aad 为附加认证数据（可传 nullptr/0）。
GRYCE_API bool aes_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* plaintext, size_t plaintext_len,
                               std::vector<uint8_t>& ciphertext, uint8_t tag[16]);

// 解密。tag 校验失败返回 false 且 plaintext 清空。
GRYCE_API bool aes_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* ciphertext, size_t ciphertext_len,
                               std::vector<uint8_t>& plaintext, const uint8_t tag[16]);

// ---------------------------------------------------------------------------
// 便捷接口（单缓冲）
// ---------------------------------------------------------------------------

// 加密到 out：iv(12) || ciphertext || tag(16)。iv 随机生成。
GRYCE_API bool aes_gcm_encrypt_buf(const uint8_t key[32],
                                   const uint8_t* aad, size_t aad_len,
                                   const uint8_t* plaintext, size_t plaintext_len,
                                   std::vector<uint8_t>& out);

// 从 data（iv || ciphertext || tag 单缓冲）解密；GCM tag 校验失败返回 false。
GRYCE_API bool aes_gcm_decrypt_buf(const uint8_t key[32],
                                   const uint8_t* aad, size_t aad_len,
                                   const uint8_t* data, size_t data_len,
                                   std::vector<uint8_t>& plaintext);

} // namespace gryce_engine::resources
