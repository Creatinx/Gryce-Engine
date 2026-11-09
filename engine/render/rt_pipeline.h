#pragma once

// ---------------------------------------------------------------------------
// rt_pipeline — 统一硬件光追管线抽象（P0 骨架）
//
// 目的：让 P1 折射/透射、P2 路径追踪 GI、P4 RT 阴影共用一套后端无关入口，
// 避免每个特性各写一份后端分支与加速结构胶水。
//
// 与既有 vk_ray_query_pass 的关系：
//   - 现有 RT 反射走 ray query（单次弹射），已稳定、保留不动。
//   - 多弹射（折射内部的反射、路径追踪的多次弹射）ray query 表达困难，
//     需要真正的光追管线（RayGen / ClosestHit / Miss + 着色器绑定表 SBT），
//     本抽象即为此而设。
//
// 后端覆盖（按当前范围）：
//   - Vulkan：VK_KHR_ray_tracing_pipeline（P1/P2/P4 目标实现）。
//   - GL / WebGL：无硬件光追，is_supported() 恒假，各特性回退现有光栅/SSR
//     路径，画面与现状零差异。
//   - D3D12：本轮不做（无 DXR），保持回退。
//
// 加速结构：BLAS/TLAS 已有实现（vk_acceleration_structure），本文件只定义
// 管线与 SBT 的抽象；后端各自实现，取不到能力时 is_supported() 为假。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "render/export.h"

namespace gryce_engine::render {

// 光追管线后端
enum class RTBackendKind { None = 0, Vulkan = 1 };

// 光追阶段的可编程着色器（Vulkan 为 SPIR-V 字节码；entry_point 供调试与校验）
struct RTShaderStage {
    std::string entry_point = "main";
    std::vector<uint32_t> spirv;
};

// 命中组：一次命中着色（closest-hit + 可选的 any-hit）
struct RTHitGroup {
    std::string name;
    RTShaderStage closest_hit;
    bool has_any_hit = false;
    RTShaderStage any_hit;
};

// 着色器绑定表：按 (ray_gen, miss, hit_group) 三段组织
struct RTShaderBindingTable {
    std::vector<RTShaderStage> ray_gen;
    std::vector<RTShaderStage> miss;
    std::vector<RTHitGroup> hit_groups;
};

// 管线描述。max_recursion_depth 是路径追踪的关键：折射内反射与多次漫反射
// 弹射都要求 > 1；单弹射反射/折射保持 1。
struct RTPipelineDesc {
    RTShaderBindingTable sbt;
    uint32_t max_recursion_depth = 1;
    // 射线负载大小（字节）：命中点、材质索引、路径权重等；路径追踪按需增大。
    uint32_t payload_size_bytes = 64;
};

// ---------------------------------------------------------------------------
// IRayTracingPipeline — 后端无关的光追管线
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API IRayTracingPipeline {
public:
    virtual ~IRayTracingPipeline() = default;

    // 后端是否具备真正的光追管线（非 ray query）。不支持时上层回退光栅/SSR。
    virtual bool is_supported() const = 0;
    virtual RTBackendKind backend_kind() const { return RTBackendKind::None; }

    // 由描述创建管线与 SBT；失败返回 false（上层回退，画面与现状零差异）。
    virtual bool create(const RTPipelineDesc& desc) = 0;
    virtual void destroy() = 0;
    virtual bool valid() const = 0;

    // 派发一次光追。后端各自把命令录到当前命令缓冲，tlas 由调用方保证已构建。
    virtual void trace(uint32_t width, uint32_t height, void* tlas) = 0;
};

} // namespace gryce_engine::render
