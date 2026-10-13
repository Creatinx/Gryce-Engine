#pragma once

// ChaCha20（RFC 7539）流加密。
// 原为 pak_bundle.cpp 内的 static 实现（GPAK v4 数据区加密），2026-10-13
// 抽取至此共享：引擎 PakWriter/PakReader 与 GryceGC 工具链的 encrypt 命令
// 共用同一实现，保证格式与行为逐字节一致。
//
// 32 字节密钥 + 12 字节 nonce + 32 位块计数器（counter 从 byte_offset/64 起）。
// chacha20_xor 支持从任意字节偏移对缓冲区加/解密（加密与解密为同一互逆操作）。
// 纯 C++ 实现，不依赖平台加密库，Windows / Linux 行为一致。

#include <cstddef>
#include <cstdint>

namespace gryce_engine::utils {

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint64_t byte_offset, uint8_t* buf, size_t len);

} // namespace gryce_engine::utils
