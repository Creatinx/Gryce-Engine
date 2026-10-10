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
// allow_uav=true 时给 DEFAULT 堆资源加 ALLOW_UNORDERED_ACCESS（读写 SSBO 必需）。
//   ★ 只有 DEFAULT 堆能带这个标志 —— UPLOAD 堆带上会被 CreateCommittedResource
//     直接拒绝，所以参数组合 upload_heap=true && allow_uav=true 是无效的（忽略）。
// 注意：DEFAULT 堆缓冲的初始态是 COMMON（不是 GENERIC_READ！），
// 作为拷贝目标要先转 COPY_DEST。这里返回 COMMON 态资源，调用方自行转换。
ID3D12Resource* create_buffer_resource(ID3D12Device* device, size_t size, bool upload_heap,
                                       bool allow_uav = false);

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
// 但 GPU 可能还在读上几帧的区间，所以不能"整块复用"。
//
// ★★ 分段轮转（关键正确性修复）★★
// 早期实现是"每帧 reset() 游标归零"，并假设"begin_frame 前 GPU 已完成上一帧"。
// 这个假设**对 frames_in_flight=3 是错的**：CPU 领先 GPU 最多 3 帧，帧 N 开始时
// 帧 N-1 很可能仍在 GPU 上执行。此时把游标归零会让帧 N 的 uniform/实例数据
// 覆盖掉帧 N-1 正在读的同一段内存 → GPU 读到撕裂数据 → Intel Arc 上直接
// DEVICE_HUNG（实测：3 帧在飞必挂，串行化 1 帧在飞完全不挂）。
//
// 正确做法：把整块按 frames_in_flight 切成 N 个**独立分段**，帧 N 固定用
// 分段 (N % N)。只有当"上次使用该分段的帧"的 GPU 围栏值已完成，才允许复用。
// 分段使用方（backend）在 begin_frame 时用 fence 值调用 begin_slot() 声明本轮
// 分段，容量不足时按需报错（调用方降级）。
class UploadRing {
public:
    bool init(ID3D12Device* device, size_t capacity);
    void shutdown();

    // 申请 size 字节（自动按 256 对齐）。返回可写指针；written_size 为对齐后
    // 实际占用（用于 offset 推进）。容量不足时返回 nullptr（调用方应扩容或降级）。
    uint8_t* allocate(size_t size, size_t& out_offset, size_t& out_aligned_size);
    // 申请并直接拷入数据，返回该区间在 GPU 上的地址。
    bool allocate_and_copy(const void* data, size_t size, D3D12_GPU_VIRTUAL_ADDRESS& out_gpu_addr);

    // ★ 进入第 slot 个帧槽（slot ∈ [0, frames_in_flight)）。把游标定位到
    //   该槽对应分段的起点。分段数在 init 时按 frames_in_flight 定。
    void begin_slot(uint32_t slot);
    // 兼容旧调用点：等价于 begin_slot(0)（但**必须**改用 begin_slot，
    // 否则又退回单分段覆写）。保留仅为过渡期编译通过。
    void reset() { begin_slot(0); }

    ID3D12Resource* resource() const { return resource_; }
    size_t capacity() const { return capacity_; }
    size_t used() const { return cursor_ - slot_base_; }
    bool is_valid() const { return resource_ != nullptr; }

    // 分段数（= frames_in_flight）。init 默认 3。
    void set_frame_slots(uint32_t n) { frame_slots_ = n ? n : 1; }

private:
    ID3D12Resource* resource_ = nullptr;
    uint8_t* mapped_ = nullptr;
    size_t capacity_ = 0;
    size_t cursor_ = 0;       // 全局游标（含 slot 偏移）
    size_t slot_base_ = 0;    // 当前 slot 分段起点
    size_t slot_capacity_ = 0;// 每个分段的容量 = capacity_ / frame_slots_
    uint32_t frame_slots_ = 3;
};

} // namespace gryce_engine::render::d3d12
