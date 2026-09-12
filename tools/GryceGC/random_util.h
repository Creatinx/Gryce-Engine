// 随机字节与 Base64URL / Hex 编码工具。
#pragma once

#include <cstdint>
#include <string>

namespace gryce_engine::gc {

// 生成 len 字节安全的随机数（Windows CNG / Linux /dev/urandom），失败返回 false。
bool random_bytes(uint8_t* out, size_t len);

// 字节 -> 小写十六进制字符串。
std::string bytes_to_hex(const uint8_t* data, size_t len);

// 32 字节随机数 -> Base64URL 编码（约 43 字符，无填充），用作 .gpkg 包文件名。
std::string random_base64_name();

} // namespace gryce_engine::gc