#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assets/asset.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// TextureData — CPU 侧纹理资源数据
// 保存解码后的像素，等待上传到 GPU。
// ---------------------------------------------------------------------------
struct TextureData : public Asset {
    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<unsigned char> pixels;

    const char* type() const override { return "TextureData"; }

    bool empty() const { return pixels.empty(); }

    // 总像素数
    int pixel_count() const { return width * height; }

    // 每像素字节数
    int bytes_per_pixel() const { return channels; }

    // 数据指针（兼容 stb_image 风格）
    const unsigned char* data() const { return pixels.data(); }
    unsigned char* data() { return pixels.data(); }
};

} // namespace gryce_engine::assets
