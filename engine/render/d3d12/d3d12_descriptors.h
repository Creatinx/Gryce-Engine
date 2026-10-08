// =============================================================================
// d3d12_descriptors — 描述符堆管理器（M3.1）
//
// D3D12 与 GL/Vulkan 的根本差异：资源**不能直接绑定**，一切绑定都通过描述符
// （descriptor）间接进行，而描述符必须从描述符堆里分配。
//
// 堆分两类，用途完全不同：
//   * SHADER_VISIBLE 堆：着色器能读到的（CBV / SRV / UAV / Sampler）。
//     这类堆**同一时刻只能有一个**被 SetDescriptorHeaps，且 GPU 只能看到
//     CURRENT 内容 —— 因此不能随意复用已被在途帧引用的槽位。
//   * NON_SHADER_VISIBLE 堆：RTV / DSV。只被 CPU 侧的 OMSetRenderTargets /
//     ClearXxxView 使用，不参与着色器采样，**不需要** SHADER_VISIBLE。
//     （M2 的 RTV 堆就是这么建的；若误传 SHADER_VISIBLE，CreateDescriptorHeap
//      会直接失败。）
//
// 本模块提供两个堆的分配/释放：
//   1) SrvHeapAllocator —— SHADER_VISIBLE，SRV/CBV/UAV 混用一块堆。
//   2) SamplerHeapAllocator —— SHADER_VISIBLE，Sampler 单独一块
//      （D3D12 规定 sampler 必须集中放在独立的 sampler 堆）。
//   RTV/DSV 由 framebuffer 与 swapchain 各自管理（它们的生命周期与 RT 绑定），
//   不纳入这里。
//
// 分配策略：自由链表（LIFO）+ 首次扩容。描述符槽位在 D3D12 里的寻址方式是
// "基址 + index * increment_size"，increment_size 由设备给出（各厂商不同！
// 常见 32 字节，Intel 可能是 64），**绝不能硬编码**。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace gryce_engine::render::d3d12 {

// ---------------------------------------------------------------------------
// DescriptorAllocator — 单块 SHADER_VISIBLE 堆上的描述符分配器
// ---------------------------------------------------------------------------
class DescriptorAllocator {
public:
    // type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV 或 _SAMPLER
    bool init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity);
    void shutdown();

    // 分配一个描述符槽位。堆满时自动扩容（重建更大的堆并把已有描述符拷贝过去）。
    // 返回 false 表示扩容失败（设备移除/内存不足）。
    bool allocate(uint32_t& out_index);

    // ★ 分配 **count 个连续**槽位，返回起始索引。
    //   描述符表（D3D12_DESCRIPTOR_RANGE）只能表达连续区间，而自由链表是 LIFO，
    //   逐个 allocate 不保证连续 —— 所以表必须用这个接口一次拿一段。
    //   失败返回 false（count==0 时返回 true 且 out_index=0）。
    bool allocate_range(uint32_t count, uint32_t& out_index);

    // 归还槽位（LIFO 复用）。调用方必须保证该槽位不再被在途帧引用。
    void free(uint32_t index);
    // 归还一段连续槽位（逐个 free）。
    void free_range(uint32_t start, uint32_t count);

    // 槽位 → CPU / GPU 句柄。
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(uint32_t index) const;

    ID3D12DescriptorHeap* heap() const { return heap_; }
    uint32_t capacity() const { return capacity_; }
    uint32_t live_count() const { return live_count_; }
    uint32_t increment_size() const { return increment_size_; }
    bool is_valid() const { return heap_ != nullptr; }

private:
    // 堆满时重建：capacity 翻倍，把旧堆 [0, capacity_) 描述符整体拷贝到新堆。
    // CopyDescriptorsSimple 是设备方法，源/目标都是描述符堆区间。
    bool grow();

    ID3D12Device* device_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type_ = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint32_t capacity_ = 0;
    uint32_t increment_size_ = 0;
    uint32_t next_unused_ = 0;
    uint32_t live_count_ = 0;
    std::vector<uint32_t> free_list_;
};

// ---------------------------------------------------------------------------
// DescriptorHeapSet — 把 SRV/CBV/UAV 与 Sampler 两个堆打包，方便一次性绑定
// ---------------------------------------------------------------------------
// 录制时每个 pass 开头调 bind(cmd) 即可（D3D12 要求 SetDescriptorHeaps 一次
// 传两个堆句柄；分两次调用是无效的，第二次会覆盖第一次）。
class DescriptorHeapSet {
public:
    // ★ 容量必须**一次给够**：这两个堆是 SHADER_VISIBLE 的，运行期扩容会重建堆，
    // 而描述符句柄里存的是绝对地址 —— 已录进命令列表的描述符表会立刻变悬空
    // （调试层崩溃 / 关掉调试层时驱动 RemoveDevice）。
    //
    // 用量估算（见 D3D12Shader::commit_textures）：每次"纹理集合变化"都要一段
    // 连续区间（pbr 的 srv/sampler 各 26 个），且**整帧不回收**（命令列表还引用
    // 着）。4096 在这个 demo 场景下曾被撑爆过（sampler 256 显然不够），
    // 8192 留了足够余量（sampler 描述符 32B × 8192 = 256KB，代价可忽略）。
    //
    // ★ sampler 有硬上限：D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE = 2048。
    //   超过它 CreateDescriptorHeap 直接 E_INVALIDARG(0x80070057)，
    //   表现为"GRender_Init: RenderContext::init failed"——整个后端起不来。
    bool init(ID3D12Device* device, uint32_t srv_capacity = 8192,
              uint32_t sampler_capacity = 2048);
    void shutdown();
    void bind(ID3D12GraphicsCommandList* cmd) const;

    DescriptorAllocator& srv() { return srv_; }
    DescriptorAllocator& sampler() { return sampler_; }
    const DescriptorAllocator& srv() const { return srv_; }
    const DescriptorAllocator& sampler() const { return sampler_; }
    bool is_valid() const { return srv_.is_valid() && sampler_.is_valid(); }

private:
    DescriptorAllocator srv_;
    DescriptorAllocator sampler_;
};

} // namespace gryce_engine::render::d3d12
