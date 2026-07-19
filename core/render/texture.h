#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// TextureFormat — 跨 API 纹理格式
// ---------------------------------------------------------------------------
enum class TextureFormat {
    RGB8,
    RGBA8,
    RGBA16F,
    RGBA32F,
    Depth16,
    Depth24,
    Depth24Stencil8,
    Depth32F,
    R8,
    RG8,
    // --- 压缩格式（块压缩，按 4x4 block 存储）---
    BC1_RGB,        // DXT1
    BC1_RGBA,       // DXT1 + 1bit alpha
    BC2,            // DXT3
    BC3,            // DXT5
    BC4,            // RGTC1 单通道
    BC5,            // RGTC2 双通道（法线）
    BC6H,           // HDR 半浮点
    BC7,            // 高质量 RGBA
    ETC2_RGB,       // ETC2 RGB8
    ETC2_RGBA,      // ETC2 RGBA8 (EAC)
    ASTC_4x4        // ASTC 4x4 LDR
};

// ---------------------------------------------------------------------------
// 纹理单元（texture unit）分配约定
// ---------------------------------------------------------------------------
// 为避免 NVIDIA 驱动因同一 texture unit 在不同 shader/pass 之间绑定不同
// 格式/目标（如 RGB8 vs RGBA16F vs Depth）的纹理而频繁重新编译 shader，
// 将不同渲染管线的采样器固定到不重叠的 slot。
namespace TextureSlots {
    // 2D 渲染器（与 PBR/tonemap/skybox 不重叠）
    constexpr int k2DAlbedo     = 0;
    constexpr int k2DNormal     = 1;
    constexpr int k2DShadow     = 7;   // depth，远离 PBR 的 color slot

    // PBR（albedo~emissive 共 7 个 slot）
    constexpr int kPBRBase      = 10;
    constexpr int kPBRAlbedo    = kPBRBase;      // 10
    constexpr int kPBRNormal    = kPBRBase + 1;  // 11
    constexpr int kPBRRoughness = kPBRBase + 2;  // 12
    constexpr int kPBRMetallic  = kPBRBase + 3;  // 13
    constexpr int kPBRAO        = kPBRBase + 4;  // 14
    constexpr int kPBRShadow    = kPBRBase + 5;  // 15
    constexpr int kPBREmissive  = kPBRBase + 6;  // 16

    // 后处理
    constexpr int kTonemapHDR   = 20;

    // 天空盒
    constexpr int kSkyboxCube   = 21;
}

// 是否为块压缩格式（每 4x4 像素一个 block：8 或 16 字节）
inline bool is_compressed_format(TextureFormat fmt) {
    return static_cast<int>(fmt) >= static_cast<int>(TextureFormat::BC1_RGB);
}

// 压缩格式每 4x4 block 的字节数（ASTC_4x4 固定 16）
inline uint32_t compressed_block_size(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::BC1_RGB:
        case TextureFormat::BC1_RGBA:
        case TextureFormat::BC4:
        case TextureFormat::ETC2_RGB:
            return 8;
        default:
            return 16;
    }
}

// 计算某一级 mip 的压缩数据字节数
inline size_t compressed_mip_size(TextureFormat fmt, int width, int height) {
    const uint32_t bw = static_cast<uint32_t>((width + 3) / 4);
    const uint32_t bh = static_cast<uint32_t>((height + 3) / 4);
    return static_cast<size_t>(bw) * bh * compressed_block_size(fmt);
}

// ---------------------------------------------------------------------------
// ITexture — 跨 API 纹理接口
// ---------------------------------------------------------------------------
enum class TextureFilter {
    Nearest,
    Linear,
    NearestMipmapNearest,
    LinearMipmapLinear
};

enum class TextureWrap {
    Repeat,
    ClampToEdge,
    ClampToBorder
};

class ITexture {
public:
    virtual ~ITexture() = default;

    virtual bool load_from_file(const std::string& path) = 0;
    virtual bool create_empty(int width, int height, int channels = 4) = 0;
    virtual bool upload_data(const void* data, int width, int height, int channels = 4) = 0;
    virtual bool create_depth(int width, int height) = 0;

    // 上传 cubemap 六个面，顺序固定为 +X,-X,+Y,-Y,+Z,-Z；
    // 每面 width×height×channels，像素按 top-down 行序，不做翻转。
    // 不支持 cubemap 的后端返回 false。
    virtual bool upload_cubemap(const void* faces[6], int width, int height, int channels = 4) {
        (void)faces; (void)width; (void)height; (void)channels;
        return false;
    }

    virtual bool is_cubemap() const { return false; }

    // 显式格式创建（PBR、阴影图、HDR 等场景）
    virtual bool create(TextureFormat format, int width, int height, const void* data = nullptr) = 0;

    // 上传块压缩纹理（2D，含全部 mip 层）。
    // mip_data[i]/mip_sizes[i] 为第 i 层数据；层尺寸按 max(1, w>>i) 推算。
    // 不支持压缩纹理的后端返回 false。
    virtual bool create_compressed(TextureFormat format, int width, int height,
                                   int mip_levels, const void* const* mip_data,
                                   const size_t* mip_sizes) {
        (void)format; (void)width; (void)height;
        (void)mip_levels; (void)mip_data; (void)mip_sizes;
        return false;
    }

    virtual void bind(uint32_t slot = 0) const = 0;
    virtual void unbind() const = 0;

    virtual void set_filter(TextureFilter min, TextureFilter mag) = 0;
    virtual void set_wrap(TextureWrap s, TextureWrap t) = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool is_valid() const = 0;
};

} // namespace gryce_engine::render
