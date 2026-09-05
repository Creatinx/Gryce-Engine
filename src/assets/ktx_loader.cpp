#include "ktx_loader.h"

#include <cstring>
#include <fstream>

#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

// KTX1 文件标识
constexpr uint8_t k_ktx_identifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// GL internalformat -> 跨 API 压缩格式
render::TextureFormat format_from_gl_internal(uint32_t internal) {
    switch (internal) {
        case 0x83F0: return render::TextureFormat::BC1_RGB;   // COMPRESSED_RGB_S3TC_DXT1_EXT
        case 0x83F1: return render::TextureFormat::BC1_RGBA;  // COMPRESSED_RGBA_S3TC_DXT1_EXT
        case 0x83F2: return render::TextureFormat::BC2;       // COMPRESSED_RGBA_S3TC_DXT3_EXT
        case 0x83F3: return render::TextureFormat::BC3;       // COMPRESSED_RGBA_S3TC_DXT5_EXT
        case 0x8DBB: return render::TextureFormat::BC4;       // COMPRESSED_RED_RGTC1
        case 0x8DBD: return render::TextureFormat::BC5;       // COMPRESSED_RG_RGTC2
        case 0x8E8F: return render::TextureFormat::BC6H;      // COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT
        case 0x8E8C: return render::TextureFormat::BC7;       // COMPRESSED_RGBA_BPTC_UNORM
        case 0x9274: return render::TextureFormat::ETC2_RGB;  // COMPRESSED_RGB8_ETC2
        case 0x9278: return render::TextureFormat::ETC2_RGBA; // COMPRESSED_RGBA8_ETC2_EAC
        case 0x93B0: return render::TextureFormat::ASTC_4x4;  // COMPRESSED_RGBA_ASTC_4x4_KHR
        default:     return render::TextureFormat::RGBA8;
    }
}

} // namespace

bool KTXLoader::load(const std::string& path, CompressedImage& out) {
    const std::string resolved = resources::ResourcePath::resolve(path);
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        GLOG_ERROR("KTXLoader: failed to open '{}'", resolved);
        return false;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        GLOG_ERROR("KTXLoader: failed to read '{}'", resolved);
        return false;
    }
    return parse(bytes.data(), bytes.size(), out);
}

bool KTXLoader::parse(const uint8_t* data, size_t size, CompressedImage& out) {
    constexpr size_t k_header_size = 12 + 13 * 4; // identifier + 13 个 uint32
    if (!data || size < k_header_size) return false;
    if (std::memcmp(data, k_ktx_identifier, 12) != 0) return false;

    const uint8_t* h = data + 12;
    const uint32_t endianness        = read_u32(h + 0);
    const uint32_t gl_type           = read_u32(h + 4);
    const uint32_t gl_format         = read_u32(h + 12);
    const uint32_t gl_internalformat = read_u32(h + 16);
    const uint32_t pixel_width       = read_u32(h + 24);
    const uint32_t pixel_height      = read_u32(h + 28);
    const uint32_t pixel_depth       = read_u32(h + 32);
    const uint32_t array_elements    = read_u32(h + 36);
    const uint32_t faces             = read_u32(h + 40);
    uint32_t mip_levels              = read_u32(h + 44);
    const uint32_t kv_bytes          = read_u32(h + 48);

    if (endianness != 0x04030201) return false;
    if (gl_type != 0 || gl_format != 0) {
        GLOG_WARN("KTXLoader: uncompressed KTX is not supported");
        return false;
    }
    if (pixel_depth != 0 || array_elements != 0 || faces != 1) {
        GLOG_WARN("KTXLoader: only 2D single-face KTX is supported");
        return false;
    }
    if (mip_levels == 0) mip_levels = 1;

    const render::TextureFormat format = format_from_gl_internal(gl_internalformat);
    if (!render::is_compressed_format(format)) {
        GLOG_WARN("KTXLoader: unsupported internalformat 0x{:04x}", gl_internalformat);
        return false;
    }

    CompressedImage result;
    result.format = format;
    result.width = static_cast<int>(pixel_width);
    result.height = static_cast<int>(pixel_height);
    result.mip_levels = static_cast<int>(mip_levels);

    size_t cursor = k_header_size + kv_bytes;
    for (uint32_t i = 0; i < mip_levels; ++i) {
        if (cursor + 4 > size) return false;
        const uint32_t image_size = read_u32(data + cursor);
        cursor += 4;
        if (cursor + image_size > size) {
            GLOG_WARN("KTXLoader: truncated mip data (level {})", i);
            return false;
        }
        result.mips.emplace_back(data + cursor, data + cursor + image_size);
        // mip 数据按 4 字节对齐填充
        cursor += image_size;
        const size_t padding = (4 - (image_size % 4)) % 4;
        cursor += padding;
    }

    out = std::move(result);
    return true;
}

} // namespace gryce_engine::assets
