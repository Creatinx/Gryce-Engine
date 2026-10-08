// =============================================================================
// d3d12_upload — 上传缓冲（staging）助手（M3.2）
//
// D3D12 没有 glBufferSubData / vkCmdUpdateBuffer 这种"直接写资源"的路径：
// 任何 CPU 侧数据要进 GPU 专用资源（DEFAULT 堆），都必须：
//     写 UPLOAD 堆缓冲 → CopyBufferRegion / CopyTextureRegion → 提交
//
// 本模块提供三类设施：
//   1) 对齐常量与辅助：D3D12 对 纹理行距（256B）与 buffer 常量（256B）有硬性
//      对齐要求，算错就会 E_INVALIDARG 或在 Close() 时炸（M2 探针踩过）。
//   2) immediate_upload_buffer()：一次性上传小数据（网格顶点/索引、纹理初值），
//      创建临时 UPLOAD 资源 → 拷进目标 → 提交并等 GPU 完成 → 释放临时资源。
//      简洁但不适合热路径（每次等待 GPU）。
//   3) UploadRing：持久映射的环形上传堆，供每帧更新（UBO/实例数据）用 ——
//      不等待 GPU，按帧轮转，避免"GPU 正在读上一帧数据时 CPU 覆写"。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace gryce_engine::render::d3d12 {

// D3D12 的纹理行距硬性对齐要求（D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256）。
constexpr size_t kTexturePitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
// 常量缓冲视图（CBV）要求大小为 256 的整数倍（D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT）。
constexpr size_t kConstantBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// 把每行 width*4 字节紧密排列的像素数据，按 row_pitch 重排到 stage 缓冲布局。
// dst 必须已按 aligned_row_pitch * height 分配。src 行距为 src_row_pitch。
void repack_rows(uint8_t* dst, size_t dst_row_pitch, const uint8_t* src, size_t src_row_pitch,
                 size_t row_bytes, uint32_t height);

// ---------------------------------------------------------------------------
// immediate_upload_buffer
// ---------------------------------------------------------------------------
// 在 UPLOAD 堆建一个 size 字节的临时缓冲，拷贝 data，返回资源（调用方负责
// Release）。若 data 为 nullptr，返回一块未初始化的临时缓冲。
// 失败返回 nullptr。
ID3D12Resource* create_upload_buffer(ID3D12Device* device, size_t size, const void* data);

// 建一块 GPU 专用缓冲资源（DEFAULT 或 UPLOAD 堆），不含任何数据。
// upload_heap=true → UPLOAD（CPU 可写），false → DEFAULT（GPU 独占）。
// 注意：DEFAULT 堆缓冲的初始态是 COMMON（不是 GENERIC_READ！），
// 作为拷贝目标要先转 COPY_DEST。这里返回 COMMON 态资源，调用方自行转换。
ID3D12Resource* create_buffer_resource(ID3D12Device* device, size_t size, bool upload_heap);

// 提交一个"拷贝命令"并阻塞等待完成，用于资源初始化这类一次性操作。
// 内部自建 allocator/list/fence，用完即销毁 —— 不要用于每帧热路径。
bool immediate_submit_copy(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12Resource* dst, uint64_t dst_offset,
                           ID3D12Resource* src, uint64_t src_offset, size_t size);

// 提交一段已经在 cmd 上录制好的拷贝命令并等待完成（纹理上传用：拷贝命令
// 涉及 footprint 计算，由调用方自己录，这里只负责提交+等待）。
bool submit_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
                     ID3D12GraphicsCommandList* cmd);

// ---------------------------------------------------------------------------
// UploadRing — 每帧轮转的持久映射上传堆
// ---------------------------------------------------------------------------
// 用途：UBO（常量缓冲）、每帧变化的实例数据。D3D12 的 UPLOAD 堆可以长期 Map，
// 但 GPU 可能还在读上几帧的区间，所以不能"整块复用"——用环形指针推进：
//   * 每帧 begin_frame() 重置游标（或按帧数轮转 N 个分块）；
//   * allocate(size) 返回一段可写内存 + 它在缓冲里的 GPU 地址。
// 简化实现：分配容量足够大（如 4MB）+ 游标单向推进；每帧调用 reset() 把游标
// 归零。只要一帧内的总量 < 容量，就不会踩到未完成的区间（配合 frames_in_flight
// 的实际延迟，容量留出 3 倍余量即可）。
class UploadRing {
public:
    bool init(ID3D12Device* device, size_t capacity);
    void shutdown();

    // 申请 size 字节（自动按 256 对齐）。返回可写指针；written_size 为对齐后
    // 实际占用（用于 offset 推进）。容量不足时返回 nullptr（调用方应扩容或降级）。
    uint8_t* allocate(size_t size, size_t& out_offset, size_t& out_aligned_size);
    // 申请并直接拷入数据，返回该区间在 GPU 上的地址。
    bool allocate_and_copy(const void* data, size_t size, D3D12_GPU_VIRTUAL_ADDRESS& out_gpu_addr);

    // 每帧开头调用：游标归零。因为 begin_frame 前 GPU 已完成上一帧的
    // （frames_in_flight 保证），此刻复用整个缓冲是安全的。
    void reset() { cursor_ = 0; }

    ID3D12Resource* resource() const { return resource_; }
    size_t capacity() const { return capacity_; }
    size_t used() const { return cursor_; }
    bool is_valid() const { return resource_ != nullptr; }

private:
    ID3D12Resource* resource_ = nullptr;
    uint8_t* mapped_ = nullptr;
    size_t capacity_ = 0;
    size_t cursor_ = 0;
};

} // namespace gryce_engine::render::d3d12
