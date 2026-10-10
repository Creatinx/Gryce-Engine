#include "d3d12_buffer.h"

#include <cstring>

#include "d3d12_upload.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

// create_buffer_resource 是共享助手，定义在 d3d12_upload.cpp（多条路径都要用）。

D3D12Buffer::~D3D12Buffer() {
    destroy();
}

void D3D12Buffer::set_context(ID3D12Device* device, ID3D12CommandQueue* queue,
                              UploadSink* sink) {
    device_ = device;
    queue_ = queue;
    sink_ = sink;
}

bool D3D12Buffer::create(size_t size, const void* data, BufferUsage usage) {
    if (!device_ || size == 0) {
        GLOG_ERROR("D3D12Buffer::create: invalid args (device={}, size={})",
                   static_cast<const void*>(device_), size);
        return false;
    }
    release();
    size_ = size;
    usage_ = usage;

    // Static → DEFAULT（GPU 独占，创建时一次性上传）
    // Dynamic/Stream → UPLOAD（CPU 每帧可写）
    //
    // ★ 例外：SSBO（storage_）。D3D12 只允许 **DEFAULT 堆**资源当 UAV，且必须显式
    //   打开 ALLOW_UNORDERED_ACCESS（UPLOAD 堆带这个标志会被 CreateCommittedResource
    //   直接拒绝）。所以 storage 缓冲一律 DEFAULT + UAV，update() 走 staging 拷贝
    //   （录制进当前帧命令列表，见 UploadSink）。
    is_upload_heap_ = (usage != BufferUsage::Static) && !storage_;

    resource_ = create_buffer_resource(device_, size, is_upload_heap_, storage_);
    if (!resource_) {
        size_ = 0;
        return false;
    }
    state_ = is_upload_heap_ ? D3D12_RESOURCE_STATE_GENERIC_READ
                             : D3D12_RESOURCE_STATE_COMMON;

    if (is_upload_heap_) {
        // 持久映射：UPLOAD 堆允许整生命周期保持映射，后续 update() 直接写。
        D3D12_RANGE no_read{0, 0};
        HRESULT hr = resource_->Map(0, &no_read, &mapped_);
        if (FAILED(hr) || !mapped_) {
            GLOG_ERROR("D3D12Buffer::create: Map failed (hr=0x{:08x})", static_cast<unsigned>(hr));
            release();
            return false;
        }
        if (data) {
            std::memcpy(mapped_, data, size);
        }
    } else if (data) {
        // DEFAULT 堆：临时 UPLOAD 缓冲 → CopyBufferRegion → 提交等待。仅创建时一次。
        ID3D12Resource* staging = create_upload_buffer(device_, size, data);
        if (!staging) {
            release();
            return false;
        }
        const bool ok = immediate_submit_copy(device_, queue_, resource_, 0, staging, 0, size);
        staging->Release();
        if (!ok) {
            GLOG_ERROR("D3D12Buffer::create: staging copy failed");
            release();
            return false;
        }
    }
    return true;
}

void D3D12Buffer::update(const void* data, size_t size, size_t offset) {
    if (!resource_ || !data || size == 0) return;
    if (offset + size > size_) {
        GLOG_WARN("D3D12Buffer::update: out of range (off={}, size={}, cap={})", offset, size, size_);
        return;
    }

    if (is_upload_heap_ && mapped_) {
        // UPLOAD 堆：直接 CPU 写。无需 fence —— 调用方（渲染线程）保证不会在
        // GPU 仍在读该区间时写（每帧更新的数据由 UploadRing 或按帧轮转承担）。
        std::memcpy(static_cast<uint8_t*>(mapped_) + offset, data, size);
        return;
    }

    // DEFAULT 堆 SSBO：把 staging 拷贝**录制进当前帧命令列表**（sink 注入）。
    // 走"提交+等待"的同步路径会让每帧更新的 SSBO（GPU 剔除的间接参数缓冲）
    // 产生 N 次 GPU 停顿 —— 那是不可接受的。
    if (sink_ && sink_->record_copy(resource_, offset, data, size, state_)) {
        return;
    }

    // 兜底：无命令列表可用（帧外调用，或非 storage 的 DEFAULT 静态缓冲）。
    // 这是"正确的慢路径"，静态缓冲本就不该频繁 update。
    ID3D12Resource* staging = create_upload_buffer(device_, size, data);
    if (!staging) return;
    immediate_submit_copy(device_, queue_, resource_, offset, staging, 0, size);
    staging->Release();
}

bool D3D12Buffer::readback(void* dst, size_t size, size_t offset) {
    if (!resource_ || !dst || size == 0 || !device_) return false;
    if (offset + size > size_) {
        GLOG_WARN("D3D12Buffer::readback: out of range");
        return false;
    }

    // READBACK 堆缓冲（大小 = 请求大小，不需要走 footprint 对齐 —— buffer 拷贝
    // 是字节级的，只有纹理拷贝才有 256B 行距约束）。
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
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

    ID3D12Resource* rb = nullptr;
    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __mingw_uuidof<ID3D12Resource>(), reinterpret_cast<void**>(&rb));
    if (FAILED(hr) || !rb) {
        GLOG_ERROR("D3D12Buffer::readback: CreateCommittedResource failed");
        return false;
    }

    if (!immediate_submit_copy(device_, queue_, rb, 0, resource_, offset, size)) {
        rb->Release();
        return false;
    }

    D3D12_RANGE range{0, size};
    void* mapped = nullptr;
    hr = rb->Map(0, &range, &mapped);
    bool ok = false;
    if (SUCCEEDED(hr) && mapped) {
        std::memcpy(dst, mapped, size);
        rb->Unmap(0, nullptr);
        ok = true;
    }
    rb->Release();
    return ok;
}

void D3D12Buffer::bind(uint32_t binding_point) const {
    // D3D12 的绑定是"root signature 槽位 + 描述符表 / 根 CBV"决定的，不存在
    // GL 式的 glBindBufferBase(index) 立即状态。这里记录意图，实际绑定由
    // 上层在 CreateShaderResourceView / root parameter 处完成。
    binding_point_ = binding_point;
}

void D3D12Buffer::destroy() {
    release();
    // ★ storage 标记只能在 destroy() 里清（不能在 release() 里清）：create() 的
    //   第一步会调 release()，而 set_storage() 在 create() 之前设置 —— 在
    //   release() 里清会把刚设好的标记连同资源一起抹掉。句柄池会复用对象，
    //   所以这里必须清，否则复用到非 storage 的 create 会误建 DEFAULT+UAV。
    storage_ = false;
    storage_read_write_ = true;
}

void D3D12Buffer::release() {
    if (resource_) {
        if (mapped_) {
            resource_->Unmap(0, nullptr);
            mapped_ = nullptr;
        }
        resource_->Release();
        resource_ = nullptr;
    }
    size_ = 0;
    is_upload_heap_ = false;
    state_ = D3D12_RESOURCE_STATE_COMMON;
}

} // namespace gryce_engine::render::d3d12
