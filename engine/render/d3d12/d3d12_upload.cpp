#include "d3d12_upload.h"

#include <cstring>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

void repack_rows(uint8_t* dst, size_t dst_row_pitch, const uint8_t* src, size_t src_row_pitch,
                 size_t row_bytes, uint32_t height) {
    if (!dst || !src) return;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dst_row_pitch,
                    src + static_cast<size_t>(y) * src_row_pitch,
                    row_bytes);
    }
}

ID3D12Resource* create_upload_buffer(ID3D12Device* device, size_t size, const void* data) {
    if (!device || size == 0) return nullptr;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* res = nullptr;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 __mingw_uuidof<ID3D12Resource>(),
                                                 reinterpret_cast<void**>(&res));
    if (FAILED(hr) || !res) {
        GLOG_ERROR("create_upload_buffer: CreateCommittedResource failed (size={}, hr=0x{:08x})",
                   size, static_cast<unsigned>(hr));
        return nullptr;
    }

    if (data) {
        void* mapped = nullptr;
        // 只读的 CPU 侧 write，无需读回；传空 range 表示"CPU 不读"。
        D3D12_RANGE no_read{0, 0};
        if (SUCCEEDED(res->Map(0, &no_read, &mapped)) && mapped) {
            std::memcpy(mapped, data, size);
            res->Unmap(0, nullptr);
        } else {
            GLOG_ERROR("create_upload_buffer: Map failed");
            res->Release();
            return nullptr;
        }
    }
    return res;
}

ID3D12Resource* create_buffer_resource(ID3D12Device* device, size_t size, bool upload_heap,
                                       bool allow_uav) {
    if (!device || size == 0) return nullptr;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = upload_heap ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // ★ UAV（读写 SSBO）需要显式打开 ALLOW_UNORDERED_ACCESS，否则
    //   CreateUnorderedAccessView 会 E_INVALIDARG。且**只有 DEFAULT 堆**允许：
    //   UPLOAD 堆资源带这个标志会被 CreateCommittedResource 直接拒绝。
    if (allow_uav && !upload_heap) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_RESOURCE_STATES initial =
        upload_heap ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource* res = nullptr;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr,
                                                 __mingw_uuidof<ID3D12Resource>(),
                                                 reinterpret_cast<void**>(&res));
    if (FAILED(hr) || !res) {
        GLOG_ERROR("create_buffer_resource: failed (size={}, upload={}, uav={}, hr=0x{:08x})", size,
                   upload_heap, allow_uav, static_cast<unsigned>(hr));
        return nullptr;
    }
    return res;
}

bool submit_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
                     ID3D12GraphicsCommandList* cmd) {
    if (!device || !queue || !cmd) return false;

    HRESULT hr = cmd->Close();
    if (FAILED(hr)) {
        GLOG_ERROR("submit_and_wait: cmd Close failed (hr=0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);

    // 建临时 fence + 事件，等这一批完成。
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __mingw_uuidof<ID3D12Fence>(),
                             reinterpret_cast<void**>(&fence));
    if (FAILED(hr) || !fence) {
        GLOG_ERROR("submit_and_wait: CreateFence failed (hr=0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const UINT64 target = 1;
    bool ok = true;
    hr = queue->Signal(fence, target);
    if (FAILED(hr) || fence->GetCompletedValue() < target) {
        hr = fence->SetEventOnCompletion(target, ev);
        if (FAILED(hr)) {
            ok = false;
        } else {
            WaitForSingleObject(ev, 10000);
        }
    }
    if (ev) CloseHandle(ev);
    fence->Release();
    return ok;
}

bool immediate_submit_copy(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12Resource* dst, uint64_t dst_offset,
                           ID3D12Resource* src, uint64_t src_offset, size_t size) {
    if (!device || !queue || !dst || !src) return false;

    ID3D12CommandAllocator* alloc = nullptr;
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                __mingw_uuidof<ID3D12CommandAllocator>(),
                                                reinterpret_cast<void**>(&alloc));
    if (FAILED(hr) || !alloc) return false;

    ID3D12GraphicsCommandList* cmd = nullptr;
    // CreateCommandList 返回时已处于 recording 态，可立即录制。
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                   __mingw_uuidof<ID3D12GraphicsCommandList>(),
                                   reinterpret_cast<void**>(&cmd));
    if (FAILED(hr) || !cmd) {
        alloc->Release();
        return false;
    }

    cmd->CopyBufferRegion(dst, dst_offset, src, src_offset, size);
    const bool ok = submit_and_wait(device, queue, cmd);

    cmd->Release();
    alloc->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// UploadRing
// ---------------------------------------------------------------------------

bool UploadRing::init(ID3D12Device* device, size_t capacity) {
    if (!device || capacity == 0) return false;
    shutdown();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = capacity;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 __mingw_uuidof<ID3D12Resource>(),
                                                 reinterpret_cast<void**>(&resource_));
    if (FAILED(hr) || !resource_) {
        GLOG_ERROR("UploadRing::init: CreateCommittedResource failed (cap={}, hr=0x{:08x})",
                   capacity, static_cast<unsigned>(hr));
        return false;
    }

    // 持久映射：整块只 Map 一次。D3D12 的 UPLOAD 堆允许常驻映射。
    D3D12_RANGE no_read{0, 0};
    hr = resource_->Map(0, &no_read, reinterpret_cast<void**>(&mapped_));
    if (FAILED(hr) || !mapped_) {
        GLOG_ERROR("UploadRing::init: Map failed (hr=0x{:08x})", static_cast<unsigned>(hr));
        resource_->Release();
        resource_ = nullptr;
        return false;
    }
    capacity_ = capacity;
    // ★ 每帧槽的容量必须向下取整到 256 的倍数。
    //   槽起点 = slot_capacity_ * slot，而 D3D12 要求**根 CBV 的 GPU 地址 256 对齐**。
    //   若 slot_capacity_ 不是 256 的倍数（4MB/3 = 1398101），槽 1/2 的起点就会
    //   落在非 256 边界上 → 该槽内所有 CBV 全部未对齐 → 调试层报
    //   "SetGraphicsRootConstantBufferView: Pointer ... incorrectly aligned"，
    //   GPU 读到垃圾常数 → 矩阵失效 → 几何画到屏幕外 → 画面周期性变黑
    //   （周期恰好 = 帧槽数 3，因为只有槽 0 恰好对齐）。
    slot_capacity_ = (capacity_ / (frame_slots_ ? frame_slots_ : 1))
                     & ~static_cast<size_t>(kConstantBufferAlignment - 1);
    cursor_ = 0;
    slot_base_ = 0;
    return true;
}

void UploadRing::shutdown() {
    if (resource_) {
        if (mapped_) {
            resource_->Unmap(0, nullptr);
            mapped_ = nullptr;
        }
        resource_->Release();
        resource_ = nullptr;
    }
    capacity_ = 0;
    slot_capacity_ = 0;
    cursor_ = 0;
    slot_base_ = 0;
}

void UploadRing::begin_slot(uint32_t slot) {
    const uint32_t slots = frame_slots_ ? frame_slots_ : 1;
    slot = slot % slots;
    slot_base_ = slot_capacity_ * static_cast<size_t>(slot);
    cursor_ = slot_base_;
}

uint8_t* UploadRing::allocate(size_t size, size_t& out_offset, size_t& out_aligned_size) {
    if (!mapped_) return nullptr;
    const size_t aligned = align_up(size, kConstantBufferAlignment);
    // 只在"本帧槽分段"内推进，绝不越界到别的槽 —— 那是跨帧并行的安全边界。
    if (cursor_ + aligned > slot_base_ + slot_capacity_) return nullptr;
    out_offset = cursor_;
    out_aligned_size = aligned;
    uint8_t* p = mapped_ + cursor_;
    cursor_ += aligned;
    return p;
}

bool UploadRing::allocate_and_copy(const void* data, size_t size,
                                   D3D12_GPU_VIRTUAL_ADDRESS& out_gpu_addr) {
    if (!data || size == 0) return false;
    size_t offset = 0, aligned = 0;
    uint8_t* p = allocate(size, offset, aligned);
    if (!p) {
        GLOG_WARN("UploadRing: out of space (need={}, used={}, cap={})", size, cursor_, capacity_);
        return false;
    }
    std::memcpy(p, data, size);
    out_gpu_addr = resource_->GetGPUVirtualAddress() + offset;
    return true;
}

} // namespace gryce_engine::render::d3d12
