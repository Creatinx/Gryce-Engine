#include "d3d12_texture.h"

#include <cstring>

#include "d3d12_upload.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

// ---------------------------------------------------------------------------
// 格式映射
// ---------------------------------------------------------------------------
DXGI_FORMAT to_dxgi_format(TextureFormat format) {
    switch (format) {
        case TextureFormat::R8:              return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::RG8:             return DXGI_FORMAT_R8G8_UNORM;
        // D3D12 没有 24bpp 格式，RGB8 一律**提升为 RGBA8**（第 4 通道补 1.0）。
        // 不能返回 UNKNOWN：那会让 create_resource 直接失败，材质纹理全部创建不了
        // （日志里成片的 "D3D12Texture::create: unsupported format 2"），
        // 而且一旦有资源真的以 UNKNOWN 落下来，后面 create_srv 的 Format=UNKNOWN
        // 会让调试层判定非法并**移除设备**。
        case TextureFormat::RGB8:            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA8:           return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::SRGB8_ALPHA8:    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::R16F:            return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RG16F:           return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA16F:         return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32F:            return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::RG32F:           return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA32F:         return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::Depth16:         return DXGI_FORMAT_D16_UNORM;
        case TextureFormat::Depth24:         return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::Depth32F:        return DXGI_FORMAT_D32_FLOAT;
        case TextureFormat::D32FS8:          return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case TextureFormat::BC1_RGB:         return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC1_RGBA:        return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC2:             return DXGI_FORMAT_BC2_UNORM;
        case TextureFormat::BC3:             return DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC4:             return DXGI_FORMAT_BC4_UNORM;
        case TextureFormat::BC5:             return DXGI_FORMAT_BC5_UNORM;
        case TextureFormat::BC6H:            return DXGI_FORMAT_BC6H_UF16;
        case TextureFormat::BC7:             return DXGI_FORMAT_BC7_UNORM;
        // ETC2 / ASTC 在 D3D12 桌面上不通用（仅移动 / 特定扩展），显式不支持。
        case TextureFormat::ETC2_RGB:
        case TextureFormat::ETC2_RGBA:
        case TextureFormat::ASTC_4x4:       return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool is_srgb_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

bool is_depth_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

uint32_t compute_mip_levels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    uint32_t m = width > height ? width : height;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

namespace {

// 每个像素的字节数（未压缩格式）。压缩格式返回 0（走 block 计算）。
uint32_t bytes_per_pixel(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8_UNORM:              return 1;
        case DXGI_FORMAT_R8G8_UNORM:            return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return 4;
        case DXGI_FORMAT_R16_FLOAT:             return 2;
        case DXGI_FORMAT_R16G16_FLOAT:          return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 8;
        case DXGI_FORMAT_R32_FLOAT:             return 4;
        case DXGI_FORMAT_R32G32_FLOAT:          return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:    return 16;
        case DXGI_FORMAT_D16_UNORM:             return 2;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     return 4;
        case DXGI_FORMAT_D32_FLOAT:             return 4;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return 8;
        default:                                return 0;
    }
}

// 对应 TextureFormat 的"默认"通道数 → 挑一个 RGBA8 之外的合适格式。
TextureFormat pick_format_for_channels(int channels, bool want_hdr, bool want_srgb = false) {
    if (want_hdr) {
        switch (channels) {
            case 1: return TextureFormat::R16F;
            case 2: return TextureFormat::RG16F;
            case 3:
            case 4: return TextureFormat::RGBA16F;
            default: return TextureFormat::RGBA16F;
        }
    }
    switch (channels) {
        case 1: return TextureFormat::R8;
        case 2: return TextureFormat::RG8;
        case 3:
        case 4:
        default: return want_srgb ? TextureFormat::SRGB8_ALPHA8 : TextureFormat::RGBA8;
    }
}

} // namespace

// RGB8(3 字节/像素紧凑排列) → RGBA8(4 字节/像素)：第 4 通道补不透明。
//
// D3D12 没有 24bpp 格式，RGB8 一律映射成 RGBA8，而 upload_subresource 是按
// "width × bpp" 从源数据按行读取的 —— 若源仍是 24bpp 紧密排列，就会按 4 字节
// 步长去读 3 字节数据，整张图错位/倾斜。所以**任何 3 通道来源都必须先展开**。
static void expand_rgb_to_rgba(const void* src, size_t pixels, std::vector<uint8_t>& out) {
    out.resize(pixels * 4);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < pixels; ++i) {
        out[i * 4 + 0] = s[i * 3 + 0];
        out[i * 4 + 1] = s[i * 3 + 1];
        out[i * 4 + 2] = s[i * 3 + 2];
        out[i * 4 + 3] = 255;
    }
}

// 深度格式的"无类型（typeless）"等价物 —— 资源必须用它创建。
//
// ★ D3D12 的硬性规则：**深度资源必须以 typeless 格式创建**，再用各自的
// 类型化视图去 bind：
//   * DSV 用 D32_FLOAT / D24_UNORM_S8_UINT
//   * SRV 用 R32_FLOAT / R24_UNORM_X8_TYPELESS
// 直接用 D32_FLOAT 建资源、再拿它建 SRV，调试层会报
//   "CreateShaderResourceView: The format (0x28, D32_FLOAT) cannot be used with a
//    ShaderResource view"
// 并且**立刻 RemoveDevice(DXGI_ERROR_INVALID_CALL)** —— 设备永久失效。
// 本引擎的 SSAO/SSR/接触阴影都要采样深度，所以这条路径必然会走到。
DXGI_FORMAT depth_typeless_format(DXGI_FORMAT typed) {
    switch (typed) {
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:                               return typed;
    }
}

// 深度资源作为**着色器资源**被采样时使用的视图格式（非深度、可过滤）。
DXGI_FORMAT depth_srv_format(DXGI_FORMAT typed) {
    switch (typed) {
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:                               return typed;
    }
}

D3D12Texture::~D3D12Texture() {
    release();
}

void D3D12Texture::set_context(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_ = device;
    queue_ = queue;
}

void D3D12Texture::release() {
    if (resource_) {
        resource_->Release();
        resource_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    mip_levels_ = 1;
    array_size_ = 1;
    sample_count_ = 1;
    format_ = DXGI_FORMAT_UNKNOWN;
    srv_format_ = DXGI_FORMAT_UNKNOWN;
    is_cubemap_ = false;
    is_depth_ = false;
    current_state_ = D3D12_RESOURCE_STATE_COMMON;
}

bool D3D12Texture::create_resource(uint32_t width, uint32_t height, uint32_t depth_or_array,
                                   uint32_t mip_levels, DXGI_FORMAT fmt, bool allow_rt,
                                   bool allow_ds, bool is_cube, bool is_volume,
                                   uint32_t samples) {
    if (!device_ || fmt == DXGI_FORMAT_UNKNOWN) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    if (is_volume) {
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.DepthOrArraySize = static_cast<UINT16>(depth_or_array);
    } else if (is_cube) {
        // cubemap 用 TEXTURE2D + 6 array slice（与 Vulkan 的 cube view 不同）。
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.DepthOrArraySize = 6;
    } else {
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.DepthOrArraySize = static_cast<UINT16>(depth_or_array);
    }
    rd.Alignment = 0;
    rd.Width = width;
    rd.Height = static_cast<UINT>(height);
    rd.MipLevels = static_cast<UINT16>(mip_levels);
    // 深度资源用 typeless 格式落地，DSV/SRV 各自用类型化视图（见上面的说明）。
    // `fmt` 仍是"逻辑/类型化"格式，用于 DSV、SRV、PSO 的 DSVFormat。
    rd.Format = is_depth_format(fmt) ? depth_typeless_format(fmt) : fmt;
    // 多样本资源：Count=N + Quality=0（标准多重采样模式，质量级别 0 恒存在）。
    // D3D12 要求多样本纹理 MipLevels=1，调用方（create_msaa）已保证。
    rd.SampleDesc.Count = static_cast<UINT>(samples > 0 ? samples : 1);
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    // 深度/模板纹理必须声明 ALLOW_DEPTH_STENCIL（否则 CreateCommittedResource 拒绝）。
    if (is_depth_format(fmt)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    } else if (allow_rt) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    // 纯采样纹理保持 FLAG_NONE 即可（既不 ALLOW_RT 也不 ALLOW_DS）。

    // 深度纹理要初始化为确定值：D3D12 的 CLEAR_VALUE。
    D3D12_CLEAR_VALUE clear_value{};
    const D3D12_CLEAR_VALUE* clear_ptr = nullptr;
    if (is_depth_format(fmt)) {
        clear_value.Format = fmt;
        clear_value.DepthStencil.Depth = 1.0f;
        clear_value.DepthStencil.Stencil = 0;
        clear_ptr = &clear_value;
    } else if (allow_rt) {
        clear_value.Format = fmt;
        clear_value.Color[0] = 0.0f;
        clear_value.Color[1] = 0.0f;
        clear_value.Color[2] = 0.0f;
        clear_value.Color[3] = 1.0f;
        clear_ptr = &clear_value;
    }

    const D3D12_RESOURCE_STATES initial =
        is_depth_format(fmt) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;

    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial,
                                                  clear_ptr, __mingw_uuidof<ID3D12Resource>(),
                                                  reinterpret_cast<void**>(&resource_));
    if (FAILED(hr) || !resource_) {
        GLOG_ERROR("D3D12Texture: CreateCommittedResource failed "
                   "({}x{}, fmt={}, mips={}, arr={}, hr=0x{:08x})",
                   width, height, static_cast<int>(fmt), mip_levels, depth_or_array,
                   static_cast<unsigned>(hr));
        resource_ = nullptr;
        return false;
    }

    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    mip_levels_ = mip_levels;
    array_size_ = is_cube ? 6 : depth_or_array;
    format_ = fmt;
    srv_format_ = is_depth_format(fmt) ? depth_srv_format(fmt) : fmt;
    is_cubemap_ = is_cube;
    is_depth_ = is_depth_format(fmt);
    sample_count_ = samples > 0 ? samples : 1;
    current_state_ = initial;
    return true;
}

// ---------------------------------------------------------------------------
// 上传：通用子资源拷贝
// ---------------------------------------------------------------------------
// D3D12 的 PlacedFootprint 要求 RowPitch 是 256 的倍数。源数据按
// width*bpp 紧密排列 → 必须重排到对齐后的 stage 行距，否则图像会斜切。
bool D3D12Texture::upload_subresource(const void* data, uint32_t width, uint32_t height,
                                      uint32_t mip, uint32_t slice, uint32_t bpp) {
    if (!device_ || !queue_ || !resource_ || !data || bpp == 0) return false;

    const D3D12_RESOURCE_DESC rd = resource_->GetDesc();
    // 该 mip 的实际尺寸（mip 链逐级减半，最小 1）。
    const uint32_t mip_w = width;
    const uint32_t mip_h = height;

    // D3D12 的 footprint 抓取必须走设备，因为需要知道该格式/尺寸下的
    // 真实对齐（不同 vendor 的 pitch 对齐可能有差异）。
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    D3D12_RESOURCE_DESC mip_desc = rd;
    mip_desc.Width = mip_w;
    mip_desc.Height = mip_h;
    mip_desc.MipLevels = 1;

    device_->GetCopyableFootprints(&mip_desc, 0, 1, 0, &footprint, &num_rows, &row_size,
                                   &total_bytes);
    if (total_bytes == 0) return false;

    // 建 staging 缓冲并把源数据重排进去（行距 = footprint.Footprint.RowPitch）。
    ID3D12Resource* staging = create_upload_buffer(device_, total_bytes, nullptr);
    if (!staging) return false;

    void* mapped = nullptr;
    D3D12_RANGE no_read{0, 0};
    if (FAILED(staging->Map(0, &no_read, &mapped)) || !mapped) {
        staging->Release();
        return false;
    }
    const size_t src_row_pitch = static_cast<size_t>(mip_w) * bpp;
    repack_rows(static_cast<uint8_t*>(mapped) + footprint.Offset,
                footprint.Footprint.RowPitch,
                static_cast<const uint8_t*>(data),
                src_row_pitch, src_row_pitch, num_rows);
    staging->Unmap(0, nullptr);

    // 录制拷贝：资源必须处于 COPY_DEST。
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    if (!begin_upload_batch(&alloc, &cmd)) {
        staging->Release();
        return false;
    }

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = resource_;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = current_state_;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    // 若已经是 COPY_DEST 就跳过（barrier 的 before/after 相同是非法的）。
    D3D12_RESOURCE_STATES copy_end_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (current_state_ != D3D12_RESOURCE_STATE_COPY_DEST) {
        cmd->ResourceBarrier(1, &to_copy);
        current_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = resource_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = mip + slice * rd.MipLevels;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = staging;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    D3D12_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = mip_w;
    box.bottom = mip_h;
    box.back = 1;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    // 拷完立刻转成 shader 可读（本实现不保留 staging 生命周期）。
    D3D12_RESOURCE_BARRIER to_srv{};
    to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_srv.Transition.pResource = resource_;
    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_srv.Transition.StateAfter = copy_end_state;
    cmd->ResourceBarrier(1, &to_srv);
    current_state_ = copy_end_state;

    const bool ok = end_upload_batch(alloc, cmd);
    staging->Release();
    return ok;
}

bool D3D12Texture::begin_upload_batch(ID3D12CommandAllocator** alloc,
                                      ID3D12GraphicsCommandList** cmd) {
    HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 __mingw_uuidof<ID3D12CommandAllocator>(),
                                                 reinterpret_cast<void**>(alloc));
    if (FAILED(hr) || !*alloc) return false;
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, *alloc, nullptr,
                                    __mingw_uuidof<ID3D12GraphicsCommandList>(),
                                    reinterpret_cast<void**>(cmd));
    if (FAILED(hr) || !*cmd) {
        (*alloc)->Release();
        *alloc = nullptr;
        return false;
    }
    return true;
}

bool D3D12Texture::end_upload_batch(ID3D12CommandAllocator* alloc,
                                    ID3D12GraphicsCommandList* cmd) {
    const bool ok = submit_and_wait(device_, queue_, cmd);
    if (cmd) cmd->Release();
    if (alloc) alloc->Release();
    return ok;
}

bool D3D12Texture::transition_to_shader_readable() {
    // 上传路径已在 upload_subresource 里逐次转换，这里只在需要时兜底。
    if (!resource_) return false;
    const D3D12_RESOURCE_STATES target =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (current_state_ == target) return true;
    if (current_state_ == D3D12_RESOURCE_STATE_COMMON) {
        // COMMON 是一切的初始态，可以直接 barrier 到目标。
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    if (!begin_upload_batch(&alloc, &cmd)) return false;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource_;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = current_state_;
    b.Transition.StateAfter = target;
    if (current_state_ != target) {
        cmd->ResourceBarrier(1, &b);
        current_state_ = target;
    }
    return end_upload_batch(alloc, cmd);
}

// ---------------------------------------------------------------------------
// ITexture：基础创建
// ---------------------------------------------------------------------------
bool D3D12Texture::create_empty(int width, int height, int channels) {
    const TextureFormat fmt = pick_format_for_channels(channels, false);
    return create(fmt, width, height, nullptr);
}

bool D3D12Texture::create(TextureFormat format, int width, int height, const void* data) {
    if (width <= 0 || height <= 0) return false;
    release();
    texture_format_ = format;
    const DXGI_FORMAT fmt = to_dxgi_format(format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        GLOG_ERROR("D3D12Texture::create: unsupported format {}", static_cast<int>(format));
        return false;
    }
    const uint32_t mips = 1;  // 单级（需要 mip 时由 create_compressed / 显式接口给）
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         1, mips, fmt, /*allow_rt=*/true, /*allow_ds=*/false,
                         /*is_cube=*/false, /*is_volume=*/false)) {
        return false;
    }
    if (data) {
        const uint32_t bpp = bytes_per_pixel(fmt);
        // RGB8 被提升成 RGBA8（D3D12 无 24bpp），源数据必须先展开到 4 通道，
        // 否则会按 4 字节步长去读 3 字节数据 → 整张图错位。
        if (format == TextureFormat::RGB8) {
            std::vector<uint8_t> rgba;
            expand_rgb_to_rgba(data, static_cast<size_t>(width) * height, rgba);
            if (!upload_subresource(rgba.data(), static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height), 0, 0, bpp)) {
                return false;
            }
            return true;
        }
        if (!upload_subresource(data, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                0, 0, bpp)) {
            return false;
        }
    }
    return true;
}

bool D3D12Texture::create_depth(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    release();
    texture_format_ = TextureFormat::Depth32F;
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         1, 1, DXGI_FORMAT_D32_FLOAT, false, true, false, false)) {
        return false;
    }
    return true;
}

bool D3D12Texture::create_msaa(TextureFormat format, int width, int height, int samples) {
    if (width <= 0 || height <= 0) return false;
    // 只接受 2 的幂且 <= 16（能力协商在 RenderPipeline 侧做，这里做最后一道防线）。
    if (samples < 2 || samples > 16 || (samples & (samples - 1)) != 0) {
        GLOG_ERROR("D3D12Texture::create_msaa: invalid samples {} (must be 2/4/8/16)", samples);
        return false;
    }
    release();
    texture_format_ = format;
    const DXGI_FORMAT fmt = to_dxgi_format(format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        GLOG_ERROR("D3D12Texture::create_msaa: unsupported format {}", static_cast<int>(format));
        return false;
    }
    // 多样本 RT：ALLOW_RENDER_TARGET + MipLevels=1 + Count=samples。
    // 不建 SRV（多样本资源不可直接采样），解析由 backend 的 ResolveSubresource 完成。
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         1, 1, fmt, /*allow_rt=*/true, /*allow_ds=*/false,
                         /*is_cube=*/false, /*is_volume=*/false,
                         static_cast<uint32_t>(samples))) {
        return false;
    }
    return true;
}

bool D3D12Texture::upload_data(const void* data, int width, int height, int channels) {
    if (!data || width <= 0 || height <= 0) return false;
    const TextureFormat fmt = pick_format_for_channels(channels, false, srgb_);
    const DXGI_FORMAT dxgi = to_dxgi_format(fmt);
    const uint32_t bpp = bytes_per_pixel(dxgi);

    // 3 通道源（24bpp 紧密排列）落到 RGBA8 目标时必须先展开：upload_subresource
    // 按 width × bpp 逐行读源，步长错配会让整张图斜切。
    std::vector<uint8_t> expanded;
    if (channels == 3 && bpp == 4) {
        expand_rgb_to_rgba(data, static_cast<size_t>(width) * height, expanded);
        data = expanded.data();
    }

    // 已有同尺寸资源就复用，否则重建。
    if (resource_ && width_ == width && height_ == height && format_ == dxgi) {
        return upload_subresource(data, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                  0, 0, bpp);
    }
    release();
    texture_format_ = fmt;
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, 1,
                         dxgi, true, false, false, false)) {
        return false;
    }
    return upload_subresource(data, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                              0, 0, bpp);
}

bool D3D12Texture::upload_data_hdr(const void* data, int width, int height, int channels) {
    if (!data || width <= 0 || height <= 0) return false;
    // HDR 走 RGBA16F（float 输入 → 半浮点存储）。注意不能用 upload_data，
    // 那条路径按 8bit UNORM 解释缓冲（BRDF LUT 曾因此整张变 0）。
    const TextureFormat fmt = pick_format_for_channels(channels, /*want_hdr=*/true);
    const DXGI_FORMAT dxgi = to_dxgi_format(fmt);
    const uint32_t bpp = bytes_per_pixel(dxgi);

    // 源是 float（32bit/通道），目标 RGBA16F（16bit/通道）→ 必须先转半浮点。
    const uint32_t src_bpp = static_cast<uint32_t>(channels) * 4u;
    const size_t row_bytes = static_cast<size_t>(width) * src_bpp;
    std::vector<uint8_t> converted(row_bytes * static_cast<size_t>(height));

    // float → half。手写转换（不引第三方）：IEEE754 单精度 → 半精度近似。
    auto f32_to_f16 = [](float f) -> uint16_t {
        uint32_t x = 0;
        std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = x & 0x7FFFFFu;
        if (exp <= 0) {
            // 下溢到 0（简化处理，HDR 场景足够）。
            return static_cast<uint16_t>(sign);
        }
        if (exp >= 31) {
            // 溢出到 Inf。
            return static_cast<uint16_t>(sign | 0x7C00u);
        }
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
    };

    const float* src = static_cast<const float*>(data);
    uint16_t* dst = reinterpret_cast<uint16_t*>(converted.data());
    const size_t total = static_cast<size_t>(width) * height * channels;
    for (size_t i = 0; i < total; ++i) {
        dst[i] = f32_to_f16(src[i]);
    }

    if (resource_ && width_ == width && height_ == height && format_ == dxgi) {
        return upload_subresource(converted.data(), static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), 0, 0, bpp);
    }
    release();
    texture_format_ = fmt;
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, 1,
                         dxgi, true, false, false, false)) {
        return false;
    }
    return upload_subresource(converted.data(), static_cast<uint32_t>(width),
                              static_cast<uint32_t>(height), 0, 0, bpp);
}

// ---------------------------------------------------------------------------
// cubemap
// ---------------------------------------------------------------------------
bool D3D12Texture::upload_cubemap(const void* faces[6], int width, int height, int channels) {
    if (!faces || width <= 0 || height <= 0) return false;
    for (int i = 0; i < 6; ++i) {
        if (!faces[i]) return false;
    }
    release();
    texture_format_ = pick_format_for_channels(channels, false);
    const DXGI_FORMAT dxgi = to_dxgi_format(texture_format_);
    const uint32_t bpp = bytes_per_pixel(dxgi);
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         6, 1, dxgi, false, false, /*is_cube=*/true, false)) {
        return false;
    }
    // 六个面 = TEXTURE2D 的 6 个 array slice（顺序 +X,-X,+Y,-Y,+Z,-Z 与 DXGI 一致）。
    // 3 通道源同样要先展开到 RGBA8（见 upload_data 的说明）。
    std::vector<uint8_t> expanded;
    for (uint32_t f = 0; f < 6; ++f) {
        const void* face = faces[f];
        if (channels == 3 && bpp == 4) {
            expand_rgb_to_rgba(faces[f], static_cast<size_t>(width) * height, expanded);
            face = expanded.data();
        }
        if (!upload_subresource(face, static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), 0, f, bpp)) {
            return false;
        }
    }
    return true;
}

bool D3D12Texture::upload_cubemap_hdr(const void* faces[6], int width, int height) {
    if (!faces || width <= 0 || height <= 0) return false;
    for (int i = 0; i < 6; ++i) {
        if (!faces[i]) return false;
    }
    release();
    texture_format_ = TextureFormat::RGBA16F;
    const DXGI_FORMAT dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const uint32_t bpp = bytes_per_pixel(dxgi);
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         6, 1, dxgi, false, false, true, false)) {
        return false;
    }

    // float → half 转换（每面独立）。
    const size_t pixels = static_cast<size_t>(width) * height * 4;
    std::vector<uint16_t> converted(pixels);
    for (uint32_t f = 0; f < 6; ++f) {
        const float* src = static_cast<const float*>(faces[f]);
        for (size_t i = 0; i < pixels; ++i) {
            uint32_t x = 0;
            std::memcpy(&x, &src[i], 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
            const uint32_t mant = x & 0x7FFFFFu;
            if (exp <= 0) {
                converted[i] = static_cast<uint16_t>(sign);
            } else if (exp >= 31) {
                converted[i] = static_cast<uint16_t>(sign | 0x7C00u);
            } else {
                converted[i] = static_cast<uint16_t>(sign |
                    (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
            }
        }
        if (!upload_subresource(converted.data(), static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), 0, f, bpp)) {
            return false;
        }
    }
    return true;
}

bool D3D12Texture::upload_cubemap_hdr_mips(const void* const* mip_faces, int mip_levels,
                                           int width, int height) {
    if (!mip_faces || mip_levels < 1 || width <= 0 || height <= 0) return false;
    release();
    texture_format_ = TextureFormat::RGBA16F;
    const DXGI_FORMAT dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const uint32_t bpp = bytes_per_pixel(dxgi);
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         6, static_cast<uint32_t>(mip_levels), dxgi, false, false, true, false)) {
        return false;
    }

    // ★ 契约（见 texture.h:199 与 vk_texture.cpp:392）：mip_faces[i] 是"第 i 级 6 个
    // 面的数据指针数组"，即**两层间接**。曾误当作"6*mip_levels 个面指针平铺数组"
    // 直接取 mip_faces[m*6+f]，于是第 0 级读到的是调用方栈上那个指针数组自身的地址，
    // 当成 float 狂读 1MB → SIGSEGV（故障地址落在调用方栈帧里）。
    for (int m = 0; m < mip_levels; ++m) {
        const uint32_t mw = static_cast<uint32_t>(width) >> m;
        const uint32_t mh = static_cast<uint32_t>(height) >> m;
        const uint32_t w = mw > 0 ? mw : 1;
        const uint32_t h = mh > 0 ? mh : 1;

        const void* const* level_faces = static_cast<const void* const*>(mip_faces[m]);
        if (!level_faces) continue;

        const size_t pixels = static_cast<size_t>(w) * h * 4;
        std::vector<uint16_t> converted(pixels);
        for (uint32_t f = 0; f < 6; ++f) {
            const float* src = static_cast<const float*>(level_faces[f]);
            if (!src) continue;
            for (size_t i = 0; i < pixels; ++i) {
                uint32_t x = 0;
                std::memcpy(&x, &src[i], 4);
                const uint32_t sign = (x >> 16) & 0x8000u;
                int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
                const uint32_t mant = x & 0x7FFFFFu;
                if (exp <= 0) {
                    converted[i] = static_cast<uint16_t>(sign);
                } else if (exp >= 31) {
                    converted[i] = static_cast<uint16_t>(sign | 0x7C00u);
                } else {
                    converted[i] = static_cast<uint16_t>(sign |
                        (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
                }
            }
            if (!upload_subresource(converted.data(), w, h, static_cast<uint32_t>(m), f, bpp)) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 压缩纹理 / 文件加载
// ---------------------------------------------------------------------------
bool D3D12Texture::create_compressed(TextureFormat format, int width, int height, int mip_levels,
                                     const void* const* mip_data, const size_t* mip_sizes) {
    if (width <= 0 || height <= 0 || mip_levels < 1) return false;
    const DXGI_FORMAT fmt = to_dxgi_format(format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        GLOG_WARN("D3D12Texture::create_compressed: unsupported format {}",
                  static_cast<int>(format));
        return false;
    }
    release();
    texture_format_ = format;
    const uint32_t mips = static_cast<uint32_t>(mip_levels);
    if (!create_resource(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         1, mips, fmt, false, false, false, false)) {
        return false;
    }

    // 压缩数据已经是 block 布局。upload_subresource 的 (width, height, bpp) 语义是
    // "紧密排列的位图"，这里把 block 网格当作"位图"传入：每 block 行 = bw 个
    // 元素，每元素 block_size 字节 → width=bw, height=bh, bpp=block_size。
    // 这样 source row pitch = bw * block_size，与压缩数据的实际行距一致。
    const uint32_t block = compressed_block_size(format);
    for (uint32_t m = 0; m < mips; ++m) {
        if (!mip_data || !mip_data[m]) continue;
        const uint32_t mw = (static_cast<uint32_t>(width) >> m) > 0
                                ? (static_cast<uint32_t>(width) >> m) : 1;
        const uint32_t mh = (static_cast<uint32_t>(height) >> m) > 0
                                ? (static_cast<uint32_t>(height) >> m) : 1;
        const uint32_t bw = (mw + 3) / 4;   // block 列数
        const uint32_t bh = (mh + 3) / 4;   // block 行数
        if (!upload_subresource(mip_data[m], bw, bh, m, 0, block)) {
            GLOG_WARN("D3D12Texture::create_compressed: mip {} upload failed", m);
            return false;
        }
    }
    return true;
}

bool D3D12Texture::load_from_file(const std::string& path) {
    // 图像解码需要 stb_image；引擎侧已有（third_party/stb）。
    // 这里只做文件存在性 → 解码 → 上传的胶水。M3 阶段先声明未实现，
    // 由 texture.cpp 的上层（若存在）负责走 stb 再调 upload_data。
    GLOG_WARN("D3D12Texture::load_from_file('{}') not implemented at backend level; "
              "decode with stb and call upload_data()", path);
    return false;
}

bool D3D12Texture::load_from_memory(const void* data, size_t size) {
    (void)data;
    (void)size;
    GLOG_WARN("D3D12Texture::load_from_memory not implemented at backend level");
    return false;
}

// ---------------------------------------------------------------------------
// 绑定 / 状态
// ---------------------------------------------------------------------------
void D3D12Texture::bind(uint32_t slot) const {
    // D3D12 无"绑定纹理到单元"的立即状态：由上层创建 SRV 描述符并填进
    // root signature 的（描述符表）槽位。slot 的意义与 TextureSlots 一致。
    (void)slot;
}

void D3D12Texture::unbind() const {}

void D3D12Texture::set_filter(TextureFilter min, TextureFilter mag) {
    // D3D12 的过滤模式在**采样器**里，不在纹理里。这里把跨 API 的
    // (min, mag) 归一化成一个 D3D12_FILTER，供上层创建 sampler 描述符时读取。
    const bool min_linear = (min == TextureFilter::Linear ||
                             min == TextureFilter::LinearMipmapLinear);
    const bool mag_linear = (mag == TextureFilter::Linear ||
                             mag == TextureFilter::LinearMipmapLinear);
    const bool mip_linear = (min == TextureFilter::LinearMipmapLinear ||
                             min == TextureFilter::NearestMipmapNearest);

    if (min_linear && mag_linear && mip_linear) {
        filter_ = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    } else if (min_linear && mag_linear) {
        filter_ = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    } else if (!min_linear && !mag_linear && !mip_linear) {
        filter_ = D3D12_FILTER_MIN_MAG_MIP_POINT;
    } else {
        // 混合情况：D3D12 有一套 MIN_MAG_MIP 的组合枚举，这里退到最接近的线性。
        filter_ = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

void D3D12Texture::set_wrap(TextureWrap s, TextureWrap t) {
    auto to_mode = [](TextureWrap w) -> D3D12_TEXTURE_ADDRESS_MODE {
        switch (w) {
            case TextureWrap::Repeat:        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case TextureWrap::ClampToEdge:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case TextureWrap::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        }
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    };
    address_u_ = to_mode(s);
    address_v_ = to_mode(t);
}

// ---------------------------------------------------------------------------
// 描述符创建
// ---------------------------------------------------------------------------
bool D3D12Texture::create_srv(D3D12_CPU_DESCRIPTOR_HANDLE dest) const {
    if (!device_ || !resource_) return false;
    // 用 srv_format_ 而不是 format_：深度纹理的资源是 typeless，直接拿
    // D32_FLOAT 建 SRV 会被判非法并**移除设备**（见 depth_srv_format 的说明）。
    const DXGI_FORMAT view_fmt =
        srv_format_ != DXGI_FORMAT_UNKNOWN ? srv_format_ : format_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (is_cubemap_) {
        // 6 个 array slice 组成 cube view（跨 mip 的 cube 用 MipLevels 表示）。
        srv.Format = view_fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = mip_levels_;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
    } else if (sample_count_ > 1) {
        // 多样本资源的 SRV 必须用 TEXTURE2DMS 维度（如 MSAA 深度 resolve shader
        // 逐样本读取）。TEXTURE2D 会被 debug layer 拒绝并触发 RemoveDevice。
        srv.Format = view_fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srv.Format = view_fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = mip_levels_;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    device_->CreateShaderResourceView(resource_, &srv, dest);
    return true;
}

bool D3D12Texture::create_rtv(D3D12_CPU_DESCRIPTOR_HANDLE dest, uint32_t mip,
                              uint32_t slice) const {
    if (!device_ || !resource_ || is_depth_) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = format_;
    if (sample_count_ > 1) {
        // 多样本资源的 RTV 必须用 TEXTURE2DMS 维度（无 MipSlice/PlaneSlice 字段）。
        // 用 TEXTURE2D 会被 debug layer 拒绝并触发 RemoveDevice。
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = mip;
        rtv.Texture2D.PlaneSlice = 0;
    }
    device_->CreateRenderTargetView(resource_, &rtv, dest);
    (void)slice;
    return true;
}

bool D3D12Texture::create_dsv(D3D12_CPU_DESCRIPTOR_HANDLE dest) const {
    if (!device_ || !resource_ || !is_depth_) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = format_;
    if (sample_count_ > 1) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Texture2D.MipSlice = 0;
    }
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device_->CreateDepthStencilView(resource_, &dsv, dest);
    return true;
}

} // namespace gryce_engine::render::d3d12
