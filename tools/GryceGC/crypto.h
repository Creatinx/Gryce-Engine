// 哈希工具：SHA-256 / SHA-512。
//
// Windows 使用 CNG（bcrypt.dll）；其余平台回落纯 C++ 实现（FIPS 180-4）。
// 行为一致：均返回小写十六进制摘要。
#pragma once

#include <cstddef>
#include <string>

namespace gryce_engine::gc {

std::string sha256_hex(const void* data, size_t len);
std::string sha512_hex(const void* data, size_t len);

} // namespace gryce_engine::gc