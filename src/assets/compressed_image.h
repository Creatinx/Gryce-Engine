#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/texture.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// CompressedImage — 块压缩纹理数据（DDS / KTX1 加载结果，2D、含全部 mip）
// ---------------------------------------------------------------------------
struct CompressedImage {
    render::TextureFormat format = render::TextureFormat::RGBA8;
    int width = 0;
    int height = 0;
    int mip_levels = 1;
    std::vector<std::vector<uint8_t>> mips; // mips[i] = 第 i 层压缩数据

    bool valid() const { return width > 0 && height > 0 && !mips.empty(); }

    // 每级 mip 数据的只读指针/尺寸数组（喂给 ITexture::create_compressed）
    void fill_mip_views(std::vector<const void*>& data, std::vector<size_t>& sizes) const {
        data.clear();
        sizes.clear();
        data.reserve(mips.size());
        sizes.reserve(mips.size());
        for (const auto& m : mips) {
            data.push_back(m.data());
            sizes.push_back(m.size());
        }
    }
};

// 按扩展名分派加载（.dds / .ktx），失败返回 false
bool load_compressed_image(const std::string& path, CompressedImage& out);

} // namespace gryce_engine::assets
