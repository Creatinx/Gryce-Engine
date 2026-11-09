// =============================================================================
// d3d12_rt_accel — DXR 加速结构（BLAS / TLAS）
//
// 与 Vulkan 侧 vk_acceleration_structure 同口径：
//   * BLAS 按 IMesh* 缓存，几何不变只建一次；
//   * TLAS 持久实例缓冲 + 帧内构建/更新；
//   * 构建命令录在当前帧命令列表上（零主机端等待），查询前靠 UAV 屏障串序。
//
// 与 Vulkan 的差异（D3D12 特有）：
//   * D3D12 的 AS 构建输入是 **GPU 缓冲**（不是 CPU 指针），所以 BLAS 需要把
//     IMesh::get_ray_tracing_geometry 的 CPU 顶点/索引先上传成 DEFAULT 堆缓冲
//     （顶点用 R32G32B32_FLOAT 紧凑排布，索引 uint32）；
//   * 结果/scratch 缓冲必须带 ALLOW_UNORDERED_ACCESS，构建后转
//     RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE（否则实例引用非法）。
//
// 失败即安全：任一 PrebuildInfo/创建失败 → valid() 为假，上层回退（RT 关闭）。
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

namespace gryce_engine::render::d3d12 {

struct RtGeometry {
    const float* positions = nullptr;   // xyz 紧凑
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
};

// 单个网格的 BLAS（几何静态，建一次后常驻）。
class D3D12BottomLevelAS {
public:
    bool build(ID3D12Device5* device5, ID3D12GraphicsCommandList4* cmd, const RtGeometry& geo);
    bool valid() const { return result_ != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const {
        return result_ ? result_->GetGPUVirtualAddress() : 0;
    }
    void destroy();

private:
    ID3D12Resource* result_ = nullptr;
    ID3D12Resource* scratch_ = nullptr;
    ID3D12Resource* vertex_buffer_ = nullptr;
    ID3D12Resource* index_buffer_ = nullptr;
    // 顶点/索引缓冲上传后处于 GENERIC_READ；构建命令录制时保持该态即可。
    //
    // 几何描述必须活到命令列表 Close：运行时在 Close 时仍会读 pGeometryDescs，
    // 放栈上会在函数返回后变成悬垂（表现为 Close 返回 E_INVALIDARG、整帧作废）。
    // 放堆上还额外保证对象被 move 进 BLAS 缓存后地址不变。
    std::unique_ptr<D3D12_RAYTRACING_GEOMETRY_DESC> geom_desc_;
};

// TLAS：实例缓冲是 UPLOAD 堆持久映射（CPU 直写），每帧重建或 refit。
class D3D12TopLevelAS {
public:
    // 实例数变化时扩容；返回 false 表示分配失败。
    bool ensure_capacity(ID3D12Device5* device5, uint32_t instance_count);
    // 把实例数据写进映射缓冲（CPU 侧），随后由 record_build 录制构建命令。
    bool update_instances(const void* instance_descs, size_t bytes);
    // 录制构建（或 refit）。force_full = true 时整树重建。
    bool record_build(ID3D12Device5* device5, ID3D12GraphicsCommandList4* cmd,
                      uint32_t instance_count, bool force_full);
    bool valid() const { return result_ != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const {
        return result_ ? result_->GetGPUVirtualAddress() : 0;
    }
    void destroy();

private:
    ID3D12Resource* result_ = nullptr;
    ID3D12Resource* scratch_ = nullptr;
    ID3D12Resource* instance_buffer_ = nullptr;
    uint32_t capacity_ = 0;
    uint64_t result_size_ = 0;
    uint64_t scratch_size_ = 0;
    bool built_once_ = false;
};

} // namespace gryce_engine::render::d3d12
