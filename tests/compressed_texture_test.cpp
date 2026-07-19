#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "assets/compressed_image.h"
#include "assets/dds_loader.h"
#include "assets/ktx_loader.h"
#include "render/texture.h"

using namespace gryce_engine;

namespace {

uint32_t u32le(uint32_t v) { return v; } // 小端（测试环境 x86/ARM 均为小端）

std::vector<uint8_t> make_dds_bc1() {
    std::vector<uint8_t> d;
    // magic
    d.insert(d.end(), reinterpret_cast<const uint8_t*>("DDS "),
             reinterpret_cast<const uint8_t*>("DDS ") + 4);
    // DDS_HEADER (124 bytes)
    std::vector<uint8_t> h(124, 0);
    *reinterpret_cast<uint32_t*>(h.data() + 0) = u32le(124);   // dwSize
    *reinterpret_cast<uint32_t*>(h.data() + 4) = u32le(0x1007); // dwFlags (CAPS|HEIGHT|WIDTH|PIXELFORMAT)
    *reinterpret_cast<uint32_t*>(h.data() + 8) = u32le(4);     // height
    *reinterpret_cast<uint32_t*>(h.data() + 12) = u32le(4);    // width
    *reinterpret_cast<uint32_t*>(h.data() + 24) = u32le(1);    // mip count
    // dwCaps / dwCaps2 at offset 104 / 108
    *reinterpret_cast<uint32_t*>(h.data() + 104) = u32le(0x1000); // texture
    // ddspf at offset 72 (size=32, flags=DDPF_FOURCC, fourcc='DXT1')
    *reinterpret_cast<uint32_t*>(h.data() + 72) = u32le(32);   // pf size
    *reinterpret_cast<uint32_t*>(h.data() + 76) = u32le(0x4);  // DDPF_FOURCC
    *reinterpret_cast<uint32_t*>(h.data() + 80) = u32le(0x31545844); // 'DXT1'
    d.insert(d.end(), h.begin(), h.end());
    // mip 0 data: one 4x4 BC1 block = 8 bytes
    const uint8_t block[8] = {0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00};
    d.insert(d.end(), block, block + 8);
    return d;
}

std::vector<uint8_t> make_ktx_etc2() {
    std::vector<uint8_t> d;
    const uint8_t id[12] = {0xAB,0x4B,0x54,0x58,0x20,0x31,0x31,0xBB,0x0D,0x0A,0x1A,0x0A};
    d.insert(d.end(), id, id + 12);
    // 13 uint32 LE
    uint32_t fields[13] = {
        0x04030201,   // endian
        0,            // glType
        1,            // glTypeSize
        0,            // glFormat
        0x9274,       // glInternalFormat (ETC2_RGB)
        0x1907,       // glBaseInternalFormat (GL_RGB)
        4,            // width
        4,            // height
        0,            // depth
        0,            // array elements
        1,            // faces
        1,            // mip levels
        0             // kv bytes
    };
    for (int i = 0; i < 13; ++i) {
        uint8_t* p = reinterpret_cast<uint8_t*>(&fields[i]);
        d.insert(d.end(), p, p + 4);
    }
    // mip 0 imageSize
    uint32_t sz = 8;
    uint8_t* p = reinterpret_cast<uint8_t*>(&sz);
    d.insert(d.end(), p, p + 4);
    // data
    const uint8_t block[8] = {0};
    d.insert(d.end(), block, block + 8);
    return d;
}

} // namespace

TEST(CompressedTextureTest, DDS_BC1_Parse) {
    auto bytes = make_dds_bc1();
    assets::CompressedImage img;
    ASSERT_TRUE(assets::DDSLoader::parse(bytes.data(), bytes.size(), img));
    EXPECT_EQ(img.format, render::TextureFormat::BC1_RGBA);
    EXPECT_EQ(img.width, 4);
    EXPECT_EQ(img.height, 4);
    ASSERT_EQ(img.mip_levels, 1);
    ASSERT_EQ(img.mips.size(), 1u);
    EXPECT_EQ(img.mips[0].size(), 8u);
}

TEST(CompressedTextureTest, KTX_ETC2_Parse) {
    auto bytes = make_ktx_etc2();
    assets::CompressedImage img;
    ASSERT_TRUE(assets::KTXLoader::parse(bytes.data(), bytes.size(), img));
    EXPECT_EQ(img.format, render::TextureFormat::ETC2_RGB);
    EXPECT_EQ(img.width, 4);
    EXPECT_EQ(img.height, 4);
    ASSERT_EQ(img.mip_levels, 1);
    ASSERT_EQ(img.mips.size(), 1u);
    EXPECT_EQ(img.mips[0].size(), 8u);
}

TEST(CompressedTextureTest, CompressedMipSize) {
    // 4x4 BC1 block = 8 bytes；下一级 1x1 仍是 8 bytes
    EXPECT_EQ(render::compressed_mip_size(render::TextureFormat::BC1_RGB, 4, 4), 8u);
    EXPECT_EQ(render::compressed_mip_size(render::TextureFormat::BC1_RGBA, 1, 1), 8u);
    // BC3 block = 16 bytes
    EXPECT_EQ(render::compressed_mip_size(render::TextureFormat::BC3, 4, 4), 16u);
    EXPECT_EQ(render::compressed_mip_size(render::TextureFormat::BC3, 1, 1), 16u);
}
