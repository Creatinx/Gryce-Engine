#pragma once

#include <cstdint>
#include <unordered_map>

#include "render/imgui_backend.h"

namespace gryce_engine::render {

namespace d3d12 { class D3D12Backend; }
using d3d12::D3D12Backend;

// ---------------------------------------------------------------------------
// D3D12ImGuiBackend — 使用 imgui_impl_dx12 绘制 ImGui
//
// 与 VulkanImGuiBackend 同构：包一层官方的 imgui_impl_* 后端，把引擎的
// 设备/命令列表交给它。ImGui 的 SRV 描述符由本类从一个**专用的** shader-visible
// 堆里分配 —— 不能借用引擎主堆，否则 ImGui 的分配会和引擎的资源句柄互相踩。
// ---------------------------------------------------------------------------
class D3D12ImGuiBackend final : public IImGuiBackend {
public:
    explicit D3D12ImGuiBackend(D3D12Backend* backend);
    ~D3D12ImGuiBackend() override;

    bool init() override;
    void shutdown() override;
    void new_frame() override;
    void render_draw_data(ImDrawData* draw_data) override;
    bool is_d3d12() const override { return true; }

    // D3D12 的 ImTextureID = D3D12_GPU_DESCRIPTOR_HANDLE（GPU 地址）。
    // 为引擎纹理在 ImGui 专用堆里创建 SRV 并缓存。
    uint64_t imgui_texture_id(ITexture* texture) const override;
    void invalidate_texture(ITexture* texture) override;

    // 运行时重建字体 GPU 纹理（字体大小热重载）。
    void rebuild_fonts() override;

private:
    // ImGui 专用 SRV 堆的简单线性分配器（ImGui 每帧会回收上一帧的分配）。
    struct SrvAllocator {
        static constexpr uint32_t k_capacity = 256;  // 同时存在的纹理上限
        uint32_t next = 0;
        uint32_t peak = 0;
    };

    D3D12Backend* backend_ = nullptr;
    bool initialized_ = false;

    // 引擎纹理 -> 在 ImGui 堆里的 SRV 槽位。D3D12 的纹理绑定必须经描述符，
    // 且 SRV 的 CPU/GPU 句柄在创建后固定，所以按纹理缓存即可。
    struct CacheEntry {
        uint32_t slot = 0;
    };
    mutable std::unordered_map<ITexture*, CacheEntry> cache_;
    mutable SrvAllocator srv_alloc_;
    static constexpr size_t k_max_cache_size = 64;
};

} // namespace gryce_engine::render
