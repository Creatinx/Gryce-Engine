#include "d3d12_descriptors.h"

#include <algorithm>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

bool DescriptorAllocator::init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                               uint32_t capacity) {
    if (!device) {
        GLOG_ERROR("DescriptorAllocator::init: null device");
        return false;
    }
    if (capacity == 0) capacity = 1;

    device_ = device;
    type_ = type;
    capacity_ = capacity;
    // 步长必须从设备查询：Intel 上可能是 64 字节，硬编码 32 会错位。
    increment_size_ = device->GetDescriptorHandleIncrementSize(type);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = type;
    hd.NumDescriptors = capacity_;
    // CBV_SRV_UAV 与 SAMPLER 都必须 SHADER_VISIBLE，否则着色器读不到。
    // （RTV/DSV 才用 FLAG_NONE —— 那两类不在这里。）
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NodeMask = 0;

    HRESULT hr = device_->CreateDescriptorHeap(&hd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                              reinterpret_cast<void**>(&heap_));
    if (FAILED(hr) || !heap_) {
        GLOG_ERROR("DescriptorAllocator::init: CreateDescriptorHeap failed (type={}, cap={}, hr=0x{:08x})",
                   static_cast<int>(type), capacity_, static_cast<unsigned>(hr));
        return false;
    }

    // 预留 free list 容量，避免分配期反复 realloc。
    free_list_.clear();
    free_list_.reserve(capacity_);
    next_unused_ = 0;
    live_count_ = 0;
    return true;
}

void DescriptorAllocator::shutdown() {
    if (heap_) {
        heap_->Release();
        heap_ = nullptr;
    }
    device_ = nullptr;
    capacity_ = 0;
    increment_size_ = 0;
    next_unused_ = 0;
    live_count_ = 0;
    free_list_.clear();
}

bool DescriptorAllocator::allocate(uint32_t& out_index) {
    if (!heap_) return false;

    if (!free_list_.empty()) {
        out_index = free_list_.back();
        free_list_.pop_back();
        ++live_count_;
        return true;
    }
    if (next_unused_ < capacity_) {
        out_index = next_unused_++;
        ++live_count_;
        return true;
    }
    // 堆满：扩容后重试一次。
    if (!grow()) return false;
    out_index = next_unused_++;
    ++live_count_;
    return true;
}

void DescriptorAllocator::free(uint32_t index) {
    if (index >= capacity_) return;
    if (live_count_ > 0) --live_count_;
    free_list_.push_back(index);
}

// ★ 连续区间分配：描述符表（D3D12_DESCRIPTOR_RANGE）只能表达连续区间，
//   而 LIFO 自由链表不保证连续，所以必须专门找一段连续的。
//   策略：把自由链表排序后扫"最长的连续 run"，够长就从中取；
//   不够就从 next_unused_ 顺序切一段（必要时扩容）。
bool DescriptorAllocator::allocate_range(uint32_t count, uint32_t& out_index) {
    out_index = 0;
    if (!heap_) return false;
    if (count == 0) return true;

    if (!free_list_.empty()) {
        std::sort(free_list_.begin(), free_list_.end());
        uint32_t run_start = free_list_[0];
        uint32_t run_len = 1;
        for (size_t i = 1; i <= free_list_.size(); ++i) {
            const bool contiguous = (i < free_list_.size() && free_list_[i] == free_list_[i - 1] + 1);
            if (contiguous) {
                ++run_len;
            } else {
                if (run_len >= count) break;
                if (i < free_list_.size()) { run_start = free_list_[i]; run_len = 1; }
            }
        }
        if (run_len >= count) {
            // 从 free_list_ 里摘掉 [run_start, run_start+count)
            const uint32_t end = run_start + count;
            free_list_.erase(std::remove_if(free_list_.begin(), free_list_.end(),
                                            [&](uint32_t v) { return v >= run_start && v < end; }),
                             free_list_.end());
            out_index = run_start;
            live_count_ += count;
            return true;
        }
    }

    while (next_unused_ + count > capacity_) {
        if (!grow()) return false;
    }
    out_index = next_unused_;
    next_unused_ += count;
    live_count_ += count;
    return true;
}

void DescriptorAllocator::free_range(uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) free(start + i);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocator::cpu_handle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = {};
    if (!heap_) return h;
    h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * increment_size_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocator::gpu_handle(uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = {};
    if (!heap_) return h;
    h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * increment_size_;
    return h;
}

bool DescriptorAllocator::grow() {
    if (!device_ || !heap_) return false;

    const uint32_t old_capacity = capacity_;
    const uint32_t new_capacity = capacity_ * 2;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = type_;
    hd.NumDescriptors = new_capacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NodeMask = 0;

    ID3D12DescriptorHeap* new_heap = nullptr;
    HRESULT hr = device_->CreateDescriptorHeap(&hd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                              reinterpret_cast<void**>(&new_heap));
    if (FAILED(hr) || !new_heap) {
        GLOG_ERROR("DescriptorAllocator::grow: CreateDescriptorHeap failed (cap {}->{}, hr=0x{:08x})",
                   old_capacity, new_capacity, static_cast<unsigned>(hr));
        return false;
    }

    // 把旧堆前 old_capacity 个描述符整体搬到新堆同下标处 —— 已发放的句柄索引
    // 因此保持有效（句柄记录的是 index，不是裸指针）。
    D3D12_CPU_DESCRIPTOR_HANDLE src = heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dst = new_heap->GetCPUDescriptorHandleForHeapStart();
    device_->CopyDescriptorsSimple(old_capacity, dst, src, type_);

    heap_->Release();
    heap_ = new_heap;
    capacity_ = new_capacity;
    // ★★★ 扩容对**已经发生出去**的句柄是灾难性的，必须当错误看待。
    // 新堆的基址变了，而 D3D12_CPU/GPU_DESCRIPTOR_HANDLE 里存的是**绝对地址**
    // （堆基址 + index*increment），不是 index。所以：
    //   * 本帧已经录进命令列表的 SetGraphicsRootDescriptorTable 句柄 → 指向已
    //     释放的旧堆内存（调试层会直接崩在 d3d12SDKLayers.dll 里；
    //     关掉调试层则是驱动判非法 → RemoveDevice(DXGI_ERROR_INVALID_CALL)）；
    //   * D3D12Shader 缓存的 srv_table_/sampler_table_ 同样变悬空。
    // 正确做法是**上电时一次给够**（见 DescriptorHeapSet::init 的容量注释），
    // 让运行期永远不会走到这里。走到这里说明容量估算不足，需要调大。
    GLOG_ERROR("DescriptorAllocator: GROWN {} -> {} (type={}) — 运行期扩容会让已录制的"
               "描述符表句柄全部失效！请调大 init() 的容量。",
               old_capacity, new_capacity, static_cast<int>(type_));
    return true;
}

// ---------------------------------------------------------------------------
// DescriptorHeapSet
// ---------------------------------------------------------------------------

bool DescriptorHeapSet::init(ID3D12Device* device, uint32_t srv_capacity,
                             uint32_t sampler_capacity) {
    if (!srv_.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srv_capacity)) {
        return false;
    }
    if (!sampler_.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_capacity)) {
        srv_.shutdown();
        return false;
    }
    return true;
}

void DescriptorHeapSet::shutdown() {
    srv_.shutdown();
    sampler_.shutdown();
}

void DescriptorHeapSet::bind(ID3D12GraphicsCommandList* cmd) const {
    if (!cmd || !is_valid()) return;
    // 必须一次性传两个堆：SetDescriptorHeaps 是"替换"语义，分两次调用会让第一次
    // 设置的堆失效（经典的"采样器读不到" / "SRV 全黑" 根因）。
    ID3D12DescriptorHeap* heaps[2] = { srv_.heap(), sampler_.heap() };
    cmd->SetDescriptorHeaps(2, heaps);
}

} // namespace gryce_engine::render::d3d12
