// =============================================================================
// d3d12_texture — ITexture 的 D3D12 实现（M3.3）
//
// 本文件承担三件事：
//   1) TextureFormat(跨 API) → DXGI_FORMAT 映射表；
//   2) 2D / Cube / Array / 3D 纹理的创建与 mip 链管理；
//   3) 上传：DEFAULT 堆纹理 + UPLOAD 堆 staging，逐 mip / 逐 array slice 拷贝。
//
// 关键纪律（D3D12 特有，GL 完全没有）：
//   * **行距必须 256 字节对齐**：源数据是 width*bpp 紧密排列的，D3D12 的
//     PlacedFootprint 要求 RowPitch 是 256 的倍数 → 必须 repack 到 stage 缓冲，
//     不能直接把源指针当 footprint（这是最常见的"上传后图像斜切/花屏"根因）。
//   * **state 转换**：纹理默认 COMMON；作为拷贝目标要 COPY_DEST，作为 shader
//     资源要 PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE，作为 RT 要
//     RENDER_TARGET，作为 DS 要 DEPTH_WRITE。
//   * mip 级数：D3D12 不自动生成 mip，需自己算 mip_levels（或后续接
//     mipmap 生成 pass）。这里按标准 floor(log2(max(w,h)))+1 计算。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>

#include "render/texture.h"

namespace gryce_engine::render::d3d12 {

// TextureFormat → DXGI_FORMAT。未知/不支持返回 DXGI_FORMAT_UNKNOWN。
DXGI_FORMAT to_dxgi_format(TextureFormat format);

// 该 DXGI 格式是否属于 sRGB 家族（决定要不要走 SRGB 变体）。
bool is_srgb_format(DXGI_FORMAT format);

// 该格式是否为深度/模板（决定 RESOURCE_FLAG 与 DSV 创建）。
bool is_depth_format(DXGI_FORMAT format);

// 计算完整 mip 层数。
uint32_t compute_mip_levels(uint32_t width, uint32_t height);

class D3D12Texture final : public ITexture {
public:
    D3D12Texture() = default;
    ~D3D12Texture() override;

    void set_context(ID3D12Device* device, ID3D12CommandQueue* queue);

    // ---- ITexture ----
    bool load_from_file(const std::string& path) override;
    bool load_from_memory(const void* data, size_t size) override;
    bool create_empty(int width, int height, int channels = 4) override;
    bool upload_data(const void* data, int width, int height, int channels = 4) override;
    bool upload_data_hdr(const void* data, int width, int height, int channels = 4) override;
    bool create_depth(int width, int height) override;

    bool upload_cubemap(const void* faces[6], int width, int height, int channels = 4) override;
    bool upload_cubemap_hdr(const void* faces[6], int width, int height) override;
    bool upload_cubemap_hdr_mips(const void* const* mip_faces, int mip_levels, int width,
                                 int height) override;
    bool is_cubemap() const override { return is_cubemap_; }

    bool create(TextureFormat format, int width, int height, const void* data = nullptr) override;
    // 多样本（MSAA）颜色附件：SampleDesc.Count=N，仅作渲染目标
    // （不建 SRV、无 mip），由 backend 的 ResolveSubresource 解析到单样本纹理。
    bool create_msaa(TextureFormat format, int width, int height, int samples) override;
    int sample_count() const override { return static_cast<int>(sample_count_); }
    bool is_multisampled() const override { return sample_count_ > 1; }
    bool create_compressed(TextureFormat format, int width, int height, int mip_levels,
                           const void* const* mip_data, const size_t* mip_sizes) override;

    void bind(uint32_t slot = 0) const override;
    void unbind() const override;
    void bind_raw_depth(uint32_t slot = 0) const override { bind(slot); }

    void set_filter(TextureFilter min, TextureFilter mag) override;
    void set_wrap(TextureWrap s, TextureWrap t) override;

    int width() const override { return width_; }
    int height() const override { return height_; }
    bool is_valid() const override { return resource_ != nullptr; }

    // ---- D3D12 侧扩展 ----
    ID3D12Resource* resource() const { return resource_; }
    DXGI_FORMAT dxgi_format() const { return format_; }
    TextureFormat texture_format() const { return texture_format_; }
    uint32_t mip_levels() const { return mip_levels_; }
    uint32_t array_size() const { return array_size_; }
    bool is_depth() const { return is_depth_; }
    // 当前的 D3D12 资源状态（用于 barrier 的 StateBefore 推断）。
    D3D12_RESOURCE_STATES current_state() const { return current_state_; }
    void set_current_state(D3D12_RESOURCE_STATES s) { current_state_ = s; }

    // 创建 SRV 描述符到给定的 CPU 句柄（供描述符管理器使用）。
    bool create_srv(D3D12_CPU_DESCRIPTOR_HANDLE dest) const;
    // 创建 RTV/DSV 描述符（供 framebuffer 用）。
    bool create_rtv(D3D12_CPU_DESCRIPTOR_HANDLE dest, uint32_t mip = 0,
                    uint32_t slice = 0) const;
    bool create_dsv(D3D12_CPU_DESCRIPTOR_HANDLE dest) const;

    // 采样器状态（bind 时由上层据此创建/查找 sampler 描述符）。
    D3D12_FILTER dxgi_filter() const { return filter_; }
    D3D12_TEXTURE_ADDRESS_MODE dxgi_address_u() const { return address_u_; }
    D3D12_TEXTURE_ADDRESS_MODE dxgi_address_v() const { return address_v_; }

private:
    void release();
    // 创建底层资源（DEFAULT 堆；depth 走 ALLOW_DEPTH_STENCIL）。
    // samples > 1 时创建多样本 RT 资源（不能带 mip、不能被采样）。
    bool create_resource(uint32_t width, uint32_t height, uint32_t depth_or_array,
                         uint32_t mip_levels, DXGI_FORMAT fmt, bool allow_rt, bool allow_ds,
                         bool is_cube, bool is_volume, uint32_t samples = 1);
    // 通用上传：把 data 按 row_pitch 对齐后拷贝进 (mip, slice) 子资源。
    bool upload_subresource(const void* data, uint32_t width, uint32_t height,
                            uint32_t mip, uint32_t slice, uint32_t bytes_per_pixel);
    // 拷贝后把整体状态转为 shader 可读。
    bool transition_to_shader_readable();
    bool begin_upload_batch(ID3D12CommandAllocator** alloc, ID3D12GraphicsCommandList** cmd);
    bool end_upload_batch(ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* cmd);

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Resource* resource_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    uint32_t mip_levels_ = 1;
    uint32_t array_size_ = 1;
    uint32_t sample_count_ = 1;   // 1 = 普通纹理；>1 = 多样本 RT（不可采样）
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    // 建 SRV 时用的视图格式。对普通纹理等于 format_；对深度纹理是它的
    // "颜色视图"等价物（D32_FLOAT → R32_FLOAT），因为深度格式不能直接做 SRV。
    DXGI_FORMAT srv_format_ = DXGI_FORMAT_UNKNOWN;
    TextureFormat texture_format_ = TextureFormat::RGBA8;
    bool is_cubemap_ = false;
    bool is_depth_ = false;

    D3D12_RESOURCE_STATES current_state_ = D3D12_RESOURCE_STATE_COMMON;

    D3D12_FILTER filter_ = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    D3D12_TEXTURE_ADDRESS_MODE address_u_ = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    D3D12_TEXTURE_ADDRESS_MODE address_v_ = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
};

} // namespace gryce_engine::render::d3d12
