#pragma once

#include <cstdint>
#include <string>

#include "assets/compressed_image.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// KTXLoader — Khronos Texture 1.0 解析（ETC2 / ASTC / BC 系列，2D 纹理）
// 仅支持压缩格式（glType==0）；不支持 cubemap / 数组 / 体积纹理。
// ---------------------------------------------------------------------------
class KTXLoader {
public:
    static bool load(const std::string& path, CompressedImage& out);
    // 从内存解析（便于单元测试）
    static bool parse(const uint8_t* data, size_t size, CompressedImage& out);
};

} // namespace gryce_engine::assets
