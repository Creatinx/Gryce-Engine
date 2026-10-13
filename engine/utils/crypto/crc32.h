#pragma once

// CRC32（IEEE 802.3，与 zlib/PNG 同一多项式 0xEDB88320）。
// 引擎内原由 gpack_bundle.cpp 与 pak_bundle.cpp 各自维护一份相同实现，
// 2026-10-13 抽取至此共享：GryceCore 与 GryceGC 工具链共用同一份代码，
// 保证工具写出的包与引擎读入时 CRC 逐字节一致。

#include <cstddef>
#include <cstdint>

namespace gryce_engine::utils {

uint32_t crc32(const uint8_t* data, size_t len);

} // namespace gryce_engine::utils
