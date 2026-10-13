#pragma once

// 密码学安全随机字节。
// 原由 pak_bundle.cpp / aes_gcm.cpp 各自维护一份相同实现（Windows CNG /
// /dev/urandom），2026-10-13 抽取至此共享：引擎与 GryceGC 工具链的 keygen、
// pak 加密 nonce、AES-GCM IV 共用同一来源。

#include <cstddef>
#include <cstdint>

namespace gryce_engine::utils {

// 成功填充 len 字节返回 true；失败返回 false（调用方应中止密钥/nonce 生成）。
bool random_bytes(uint8_t* buf, size_t len);

} // namespace gryce_engine::utils
