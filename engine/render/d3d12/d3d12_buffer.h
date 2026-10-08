// =============================================================================
// d3d12_buffer — IBuffer 的 D3D12 实现（M3.2）
//
// IBuffer 是 SSBO/UBO 的统一抽象（顶点/索引缓冲另有 IMesh 承担）。
//
// D3D12 的缓冲同步纪律：
//   * DEFAULT 堆：GPU 独占，速度最快，但 CPU 不能直接写 → 必须经 staging 拷贝。
//     适合 BufferUsage::Static（光照表、骨骼 palette 等一次性写入的数据）。
//   * UPLOAD 堆：CPU 可写、GPU 可读，但 GPU 端带宽较低。适合 Dynamic/Stream。
//     D3D12 不允许对 UPLOAD 资源做 COPY_DEST/COPY_SOURCE 之外的 GPU 写，也不
//     允许把它当 RT/DS/UAV —— 因此"每帧更新的 SSBO"要用 UPLOAD 堆 + 持久映射。
//
// 本实现的取舍（M3 阶段求正确性而非极致性能）：
//   Static  → DEFAULT 堆 + 创建时一次 staging 拷贝（立即提交并等待）
//   其它    → UPLOAD 堆 + 持久映射，update() 直接 memcpy（无额外提交）
//
// readback() 用 READBACK 堆 + 一次提交 + fence 等待。注意 M2 探针踩过的坑：
// 拷贝目标的缓冲大小必须与 footprint 匹配，否则报错点在 Close() 而非 Copy 处。
// =============================================================================
#pragma once

#include <cstddef>

#include <d3d12.h>

#include "render/buffer.h"

namespace gryce_engine::render::d3d12 {

class D3D12Device;

class D3D12Buffer final : public IBuffer {
public:
    D3D12Buffer() = default;
    ~D3D12Buffer() override;

    // 后端在创建时注入的设备上下文。D3D12 资源无法脱离设备创建，
    // 而 IBuffer 的工厂签名（create_buffer()）不带设备参数，故用注入。
    void set_context(ID3D12Device* device, ID3D12CommandQueue* queue);

    bool create(size_t size, const void* data, BufferUsage usage) override;
    void update(const void* data, size_t size, size_t offset) override;
    bool readback(void* dst, size_t size, size_t offset) override;
    void bind(uint32_t binding_point) const override;
    void destroy() override;

    size_t size() const override { return size_; }
    bool is_valid() const override { return resource_ != nullptr; }

    // ---- D3D12 侧扩展（供 pipeline / 描述符创建使用）----
    ID3D12Resource* resource() const { return resource_; }
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const {
        return resource_ ? resource_->GetGPUVirtualAddress() : 0;
    }
    BufferUsage usage() const { return usage_; }
    uint32_t last_binding() const { return binding_point_; }  // bind() 最近一次记录

private:
    void release();

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Resource* resource_ = nullptr;
    void* mapped_ = nullptr;
    size_t size_ = 0;
    BufferUsage usage_ = BufferUsage::Static;
    bool is_upload_heap_ = false;
    // bind() 在 D3D12 下不是立即生效的操作（真正的绑定走 root signature /
    // 描述符表），这里只记录意图，供上层查询。
    mutable uint32_t binding_point_ = 0;
};

} // namespace gryce_engine::render::d3d12
