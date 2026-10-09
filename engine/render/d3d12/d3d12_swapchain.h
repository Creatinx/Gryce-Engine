// =============================================================================
// d3d12_swapchain — D3D12 交换链 + RTV 堆 + 每帧命令分配器/围栏（M2）
//
// 设计要点（与 Vulkan 侧 vk_swapchain 对齐但形态不同）：
//   * D3D12 无 VkSurface/renderPass：HWND 直接建 IDXGISwapChain3，FLIP_DISCARD
//     模式（Win10+ 推荐，DXGI_SWAP_EFFECT_FLIP_DISCARD），呈现在 end_frame；
//   * RTV 堆：每个 back buffer 一个 RTV 描述符，堆是 SHADER_VISIBLE 吗？
//     —— 不需要。RTV/DSV 都是 NON_SHADER_VISIBLE 堆（只有 CBV_SRV_UAV 才需要
//     SHADER_VISIBLE）。这里用 RTV + DSV 两个独立堆，简化偏移计算。
//   * 帧同步：frames_in_flight=3，每槽一个 CommandAllocator + 一个 fence 值。
//     begin_frame 里 wait 当前槽的 fence 值（GPU 已跑完它）→ Reset allocator/CL。
//   * 围栏只用一个 ID3D12Fence + 一组递增的 fence value（不是每槽一个 fence）。
//   * 队列必须显式 Flush（无隐含同步），所有跨帧依赖靠 fence 值。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "d3d12_loader.h"

namespace gryce_engine::render::d3d12 {

class D3D12Swapchain {
public:
    // hwnd 由 backend 传入；queue 用于 Present（必须与渲染命令同队列）。
    bool init(Loader& loader, ID3D12Device* device, ID3D12CommandQueue* queue,
              HWND hwnd, uint32_t width, uint32_t height, uint32_t frame_count);
    void shutdown();

    // 窗口尺寸变化时重建（销毁 back buffer 引用 + RTV + 命令分配器关联）。
    bool recreate(uint32_t width, uint32_t height);

    // ---- 帧循环 ----
    // 等待"当前帧槽"的 GPU 完成，Reset 该槽 allocator 与命令列表，进入 recording。
    // 返回 false 表示本帧应跳过（设备移除等致命错误）。
    bool begin_frame();
    // Close 命令列表 → ExecuteCommandLists → Present。返回 false 表示需要 recreate。
    bool end_frame();

    // ---- 访问器 ----
    ID3D12GraphicsCommandList* command_list() const { return command_list_; }
    ID3D12CommandAllocator* allocator() const { return allocators_[current_frame_]; }
    ID3D12Resource* back_buffer() const { return back_buffers_[current_index_]; }
    // 按索引取 back buffer（截图回读用：Present 之后 current index 已变，
    // 必须按"本帧展示的那个"索引回读，见 D3D12Backend::end_frame 的说明）。
    ID3D12Resource* back_buffer_at(uint32_t index) const {
        return index < back_buffers_.size() ? back_buffers_[index] : nullptr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv() const;
    // 当前帧在途槽位（0..frames_in_flight-1），供 backend 的围栏/资源复用使用
    int current_frame_index() const { return current_frame_; }
    // 当前 back buffer 索引（0..buffer_count-1），Present 后由 GetCurrentBackBufferIndex 更新
    uint32_t current_back_buffer_index() const { return current_index_; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t buffer_count() const { return static_cast<uint32_t>(back_buffers_.size()); }
    bool vsync_enabled() const { return vsync_; }
    void set_vsync_enabled(bool enabled) { vsync_ = enabled; }

    // 等待所有在途帧完成（销毁资源 / shutdown 前调用）。
    void wait_all();

    // GPU 已完成到的围栏值。用于资源延迟释放：销毁的资源先记下"当前提交值"，
    // 等 GetCompletedValue() >= 该值时才真正 Release（D3D12 不允许销毁仍被
    // 已提交命令列表引用的资源 —— 否则报 "resource ... deleted prior to
    // executing the command list" 并可能触发设备丢失/TDR）。
    UINT64 completed_value() const { return fence_ ? fence_->GetCompletedValue() : 0; }
    // 当前已"交给 GPU"的最新围栏值（提交后即递增；用作延迟释放的时间戳）。
    UINT64 last_submitted_value() const { return next_fence_value_ - 1; }

private:
    bool create_swapchain();
    bool create_rtv_heap();
    bool create_command_objects();
    bool create_fence();
    void release_target_objects();
    // 取 back buffer 引用（init 与 recreate 共用）
    bool acquire_back_buffers();

    // 推进围栏：把当前 fence value 交给 GPU，供下一轮 wait。
    void signal_and_advance();

    Loader* loader_ = nullptr;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    HWND hwnd_ = nullptr;

    IDXGISwapChain3* swapchain_ = nullptr;
    ID3D12DescriptorHeap* rtv_heap_ = nullptr;
    UINT rtv_descriptor_size_ = 0;
    std::vector<ID3D12Resource*> back_buffers_;

    // 每帧槽资源
    std::vector<ID3D12CommandAllocator*> allocators_;
    std::vector<UINT64> frame_fence_values_;   // 每槽"最近一次提交"的围栏值
    ID3D12GraphicsCommandList* command_list_ = nullptr;

    ID3D12Fence* fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 next_fence_value_ = 1;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frame_count_ = 3;   // frames in flight
    uint32_t current_frame_ = 0;
    uint32_t current_index_ = 0; // back buffer index
    bool vsync_ = true;
    bool recording_ = false;
};

} // namespace gryce_engine::render::d3d12
