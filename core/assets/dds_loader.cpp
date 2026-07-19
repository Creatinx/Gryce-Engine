#include "dds_loader.h"

#include <cstring>
#include <fstream>

#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t fourcc(const char c[4]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(c[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c[3])) << 24);
}

// DX10 扩展头里的 DXGI_FORMAT
constexpr uint32_t k_dxgi_bc1_unorm = 71;
constexpr uint32_t k_dxgi_bc2_unorm = 74;
constexpr uint32_t k_dxgi_bc3_unorm = 77;
constexpr uint32_t k_dxgi_bc4_unorm = 80;
constexpr uint32_t k_dxgi_bc5_unorm = 83;
constexpr uint32_t k_dxgi_bc6h_uf16 = 95;
constexpr uint32_t k_dxgi_bc6h_sf16 = 96;
constexpr uint32_t k_dxgi_bc7_unorm = 98;

render::TextureFormat format_from_dxgi(uint32_t dxgi) {
    switch (dxgi) {
        case k_dxgi_bc1_unorm:  return render::TextureFormat::BC1_RGBA;
        case k_dxgi_bc2_unorm:  return render::TextureFormat::BC2;
        case k_dxgi_bc3_unorm:  return render::TextureFormat::BC3;
        case k_dxgi_bc4_unorm:  return render::TextureFormat::BC4;
        case k_dxgi_bc5_unorm:  return render::TextureFormat::BC5;
        case k_dxgi_bc6h_uf16:
        case k_dxgi_bc6h_sf16:  return render::TextureFormat::BC6H;
        case k_dxgi_bc7_unorm:  return render::TextureFormat::BC7;
        default:                return render::TextureFormat::RGBA8;
    }
}

} // namespace

bool DDSLoader::load(const std::string& path, CompressedImage& out) {
    const std::string resolved = resources::ResourcePath::resolve(path);
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        GLOG_ERROR("DDSLoader: failed to open '{}'", resolved);
        return false;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        GLOG_ERROR("DDSLoader: failed to read '{}'", resolved);
        return false;
    }
    return parse(bytes.data(), bytes.size(), out);
}

bool DDSLoader::parse(const uint8_t* data, size_t size, CompressedImage& out) {
    // magic(4) + DDS_HEADER(124)
    constexpr size_t k_header_size = 4 + 124;
    if (!data || size < k_header_size) return false;
    if (std::memcmp(data, "DDS ", 4) != 0) return false;

    const uint8_t* h = data + 4;
    const uint32_t header_size = read_u32(h + 0);
    if (header_size != 124) return false;

    const uint32_t height = read_u32(h + 8);
    const uint32_t width = read_u32(h + 12);
    uint32_t mip_count = read_u32(h + 24);
    if (mip_count == 0) mip_count = 1;

    // ddspf: offset 76 in header -> dwFlags at +0, dwFourCC at +4
    const uint8_t* pf = h + 76;
    const uint32_t pf_flags = read_u32(pf + 0);
    const uint32_t pf_fourcc = read_u32(pf + 4);
    constexpr uint32_t k_ddpf_fourcc = 0x4;
    if ((pf_flags & k_ddpf_fourcc) == 0) {
        GLOG_WARN("DDSLoader: uncompressed DDS pixel formats are not supported");
        return false;
    }

    // dwCaps2: offset 108 in header；cubemap 标记
    const uint32_t caps2 = read_u32(h + 108);
    if (caps2 & 0x200) { // DDSCAPS2_CUBEMAP
        GLOG_WARN("DDSLoader: cubemap DDS is not supported");
        return false;
    }

    render::TextureFormat format = render::TextureFormat::RGBA8;
    size_t data_offset = k_header_size;

    if (pf_fourcc == fourcc("DXT1")) {
        format = render::TextureFormat::BC1_RGBA;
    } else if (pf_fourcc == fourcc("DXT3")) {
        format = render::TextureFormat::BC2;
    } else if (pf_fourcc == fourcc("DXT5")) {
        format = render::TextureFormat::BC3;
    } else if (pf_fourcc == fourcc("ATI1") || pf_fourcc == fourcc("BC4U")) {
        format = render::TextureFormat::BC4;
    } else if (pf_fourcc == fourcc("ATI2") || pf_fourcc == fourcc("BC5U")) {
        format = render::TextureFormat::BC5;
    } else if (pf_fourcc == fourcc("DX10")) {
        if (size < k_header_size + 20) return false;
        const uint32_t dxgi = read_u32(data + k_header_size);
        format = format_from_dxgi(dxgi);
        data_offset += 20;
    } else {
        GLOG_WARN("DDSLoader: unsupported FourCC 0x{:08x}", pf_fourcc);
        return false;
    }
    if (!render::is_compressed_format(format)) {
        GLOG_WARN("DDSLoader: unsupported DXGI format");
        return false;
    }

    // 逐层读取 mip 数据
    CompressedImage result;
    result.format = format;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.mip_levels = static_cast<int>(mip_count);

    size_t cursor = data_offset;
    for (uint32_t i = 0; i < mip_count; ++i) {
        const int mw = static_cast<int>(width >> i) > 0 ? static_cast<int>(width >> i) : 1;
        const int mh = static_cast<int>(height >> i) > 0 ? static_cast<int>(height >> i) : 1;
        const size_t mip_bytes = render::compressed_mip_size(format, mw, mh);
        if (cursor + mip_bytes > size) {
            GLOG_WARN("DDSLoader: truncated mip data (level {})", i);
            return false;
        }
        result.mips.emplace_back(data + cursor, data + cursor + mip_bytes);
        cursor += mip_bytes;
    }

    out = std::move(result);
    return true;
}

} // namespace gryce_engine::assets
