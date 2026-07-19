#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assets/asset.h"
#include "render/texture.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// TextureData — CPU 侧纹理资源数据
// 三种形态之一：
//   1) LDR 8-bit（pixels，channels 通道）
//   2) HDR float（is_float=true，float_pixels，RGBA32F）
//   3) 块压缩（is_compressed=true，mips + compressed_format）
// ---------------------------------------------------------------------------
struct TextureData : public Asset {
    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<unsigned char> pixels;

    // HDR 浮点（.hdr / .exr）：RGBA 四通道 float
    bool is_float = false;
    std::vector<float> float_pixels;

    // 块压缩（.dds / .ktx）
    bool is_compressed = false;
    render::TextureFormat compressed_format = render::TextureFormat::RGBA8;
    int mip_levels = 1;
    std::vector<std::vector<uint8_t>> mips;

    const char* type() const override { return "TextureData"; }

    bool empty() const {
        return pixels.empty() && float_pixels.empty() && mips.empty();
    }

    // 总像素数
    int pixel_count() const { return width * height; }

    // 每像素字节数
    int bytes_per_pixel() const { return is_float ? 4 * static_cast<int>(sizeof(float)) : channels; }

    // 数据指针（兼容 stb_image 风格；仅 LDR 形态有效）
    const unsigned char* data() const { return pixels.data(); }
    unsigned char* data() { return pixels.data(); }
    // 为 ITexture::create_compressed 准备 mip 指针/尺寸数组
    void fill_mip_views(std::vector<const void*>& data, std::vector<size_t>& sizes) const {
        data.clear(); sizes.clear();
        data.reserve(mips.size());
        sizes.reserve(mips.size());
        for (const auto& m : mips) {
            data.push_back(m.data());
            sizes.push_back(m.size());
        }
    }
};

} // namespace gryce_engine::assets
