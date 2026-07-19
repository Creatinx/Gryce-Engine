#pragma once

#include <cstdint>
#include <string>

#include "assets/compressed_image.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// DDSLoader — DirectDraw Surface 解析（BC1~BC7，2D 纹理）
// 支持经典 FourCC（DXT1/3/5、ATI1/2、BC4U/5U）与 DX10 扩展头（DXGI BC6H/BC7）。
// 不支持 cubemap / 体积纹理 / 非压缩像素格式（返回 false）。
// ---------------------------------------------------------------------------
class DDSLoader {
public:
    static bool load(const std::string& path, CompressedImage& out);
    // 从内存解析（便于单元测试）
    static bool parse(const uint8_t* data, size_t size, CompressedImage& out);
};

} // namespace gryce_engine::assets
