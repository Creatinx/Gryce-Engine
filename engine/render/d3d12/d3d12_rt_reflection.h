// =============================================================================
// d3d12_rt_reflection — D3D12 硬件光追反射（DXR Tier 1.1 Inline Raytracing）
//
// 与 Vulkan vk_ray_query_pass 同一套合约，但不分三段：
//   * Vulkan 分 trace / temporal / composite 是为半分辨率追踪 + 时域降噪；
//   * 这里默认全分辨率追踪，trace 与 composite 合并为**一趟 compute**：
//     逐像素重建世界坐标 → 反射射线（TraceRayInline）→ 屏幕内取 HDR、否则环境
//     cube → 按 cover*ρ 与 HDR 混合，直接写输出 UAV。
//   * 输出图与 HDR 同格式（RGBA16F），由 CopyResource 覆盖回 HDR 颜色。
//
// 关键正确性：
//   * 命中点屏幕采样做**遮挡校验**（与 Vulkan 同一口径）：线性距离相对差 > 6%
//     视为被更近几何遮挡，回退环境 —— 这是"漏光"的根因，不能省。
//   * 合成按 `cover*ρ` 加权（ρ = HDR alpha 的反射率），不是整块替换，与 SSR 同口径。
//
// 失败即安全：init 任一步失败 → valid()=false，后端 no-op，画面与未开启零差异。
// =============================================================================
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "math/math.h"
#include "render/shader.h"
#include "d3d12_rt_accel.h"

namespace gryce_engine::render {
class IMesh;
}

namespace gryce_engine::render::d3d12 {

class D3D12Backend;
class D3D12Texture;

struct RtDrawInstance {
    IMesh* mesh = nullptr;
    math::Matrix4f transform;
};

class D3D12RtReflection {
public:
    D3D12RtReflection() = default;
    ~D3D12RtReflection() { shutdown(); }

    // 创建 device5 / 根签名 / 计算 PSO / 输出图 / 描述符区间 / 兜底资源。
    // 设备不支持 Tier 1.1 或 DXC/PSO 失败时返回 false（后端回退，RT 关闭）。
    bool init(D3D12Backend* backend);
    void shutdown();
    bool valid() const { return valid_; }

    // 跑一次反射：构建/更新 TLAS → 派发 trace+composite → 拷回 HDR。
    void render(ID3D12GraphicsCommandList* cmd,
                D3D12Texture* color, D3D12Texture* depth, D3D12Texture* normal,
                D3D12Texture* env, D3D12Texture* env_prefilter,
                int width, int height,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& inv_view_proj,
                const math::Vector3f& camera_pos,
                const PostProcessParams& pp,
                const std::vector<RtDrawInstance>& instances);

private:
    bool ensure_output(int width, int height);
    void destroy_output();
    bool create_root_signature();
    bool create_pipeline();

    D3D12Backend* backend_ = nullptr;
    ID3D12Device5* device5_ = nullptr;
    bool valid_ = false;

    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;

    // 每帧 uniform（256B，UPLOAD 持久映射）
    ID3D12Resource* cb_ = nullptr;
    void* cb_mapped_ = nullptr;

    // 输出（RGBA16F，UAV + COPY_SOURCE）
    ID3D12Resource* out_ = nullptr;
    int out_w_ = 0, out_h_ = 0;

    // 兜底：1x1 立方体（无环境时占位，envIntensity 门控不采样）
    ID3D12Resource* fallback_cube_ = nullptr;

    // 描述符区间（SRV t0..t5 连续 6 个；UAV u0 1 个）
    uint32_t srv_base_ = UINT32_MAX;
    uint32_t uav_base_ = UINT32_MAX;

    // BLAS 缓存（按 IMesh*）
    std::unordered_map<IMesh*, D3D12BottomLevelAS> blas_cache_;
    D3D12TopLevelAS tlas_;

    uint64_t frame_index_ = 0;
};

} // namespace gryce_engine::render::d3d12
