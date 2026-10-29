#pragma once

// ---------------------------------------------------------------------------
// vk_ray_query_pass — Vulkan 硬件光追反射（P2.2，ray query；P2.3 提质）
//
// 自包含的光线查询（ray query）计算通道：用 VK_KHR_ray_query 在 TLAS 上做
// 反射射线求交，合成结果由后端拷回 HDR 颜色。P2.3 起分两段派发：
//   1) trace（半分辨率，分辨率档 pp.rt_reflection_resolution_scale）：逐像素
//      反射射线，输出 (反射色 RGB, 混合系数 A)。命中按实例 roughness/metallic
//      做菲涅尔衰减；粗糙命中走 prefilter 环境 mip，未命中采样环境立方体贴图。
//   2) composite（全分辨率）：双线性上采样 trace 结果并与全分辨率 HDR 颜色
//      混合 —— 反射是低频内容可降采样，画面其余部分保持原始锐度。
//
// 设计要点：
//   - 仅 GRYCE_HAS_VULKAN 下编译；调用方（VulkanBackend）必须已确认
//     supports_ray_tracing() 为真，否则不会实例化本类。
//   - 着色器内联为 C++ 字符串，经 compile_glsl_to_spirv 编译（shaderc 缺失时
//     优雅失败，本通道置为无效，上层回退 SSR，行为与现状零差异）。
//   - 射线查询本身在着色器内完成（GLSL rayQueryEXT），不需要 vkCmdTraceRays 等
//     主机端 KHR 命令，因此无需解析额外 PFN。
//   - TLAS 构建命令由后端先记录进同一命令缓冲（见
//     VkTopLevelAccelerationStructure::record_build），本通道只消费。
// ---------------------------------------------------------------------------

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.h"
#include "render/shader.h"
#include "render/vulkan/vk_buffer.h"

namespace gryce_engine::render {

class VulkanDevice;
class VulkanBuffer;

// 传入着色器的反射 uniform（std140 对齐：mat4 16 字节、vec4 16 字节）。
struct RayQueryUniforms {
    math::Matrix4f inv_view_proj;        // 重建世界坐标
    math::Matrix4f view_proj;            // 命中点重投影回屏幕
    math::Vector4f cam_pos_max_dist;     // xyz=相机世界坐标, w=最大追踪距离
    math::Vector4f resolution_strength;  // xy=本段派发分辨率, z=反射强度, w=模式(0 trace/1 composite)
    math::Vector4f env_params;           // x=深度纹素缩放(全/半), y=prefilter 最大 mip,
                                         // z=环境强度(0=无环境), w=保留
};

class VkRayQueryPass {
public:
    VkRayQueryPass() = default;
    ~VkRayQueryPass() { shutdown(); }

    VkRayQueryPass(const VkRayQueryPass&) = delete;
    VkRayQueryPass& operator=(const VkRayQueryPass&) = delete;
    VkRayQueryPass(VkRayQueryPass&&) noexcept;
    VkRayQueryPass& operator=(VkRayQueryPass&&) noexcept;

    // 编译着色器、建计算管线 / 描述符集 / uniform 缓冲 / 一次性命令池。
    // 成功返回 true；shaderc 不可用或特性缺失时返回 false（上层回退 SSR）。
    bool init(VulkanDevice* device);
    void shutdown();

    bool valid() const { return valid_; }

    // 跑一次光线查询反射（trace + composite 两段派发），最终结果写入内部
    // 全分辨率合成图（composite_image()），由后端拷回 HDR 颜色。
    // tlas 为空（场景无几何实例）时直接整帧 passthrough，不产生反射。
    // depth_view / lit_view 为 HDR 深度与颜色的图像视图；env_view/env_sampler
    // 与 prefilter_view/prefilter_sampler 为 IBL 环境立方体贴图（无效句柄 =
    // 无环境，未命中方向不产生反射）；params_buffer 为每实例材质参数
    // (roughness, metallic) 的 SSBO，按 instanceCustomIndex 索引。
    void render(VkCommandBuffer cmd,
                VkAccelerationStructureKHR tlas,
                VkImageView depth_view,
                VkImageView lit_view,
                VkImageView env_view, VkSampler env_sampler,
                VkImageView prefilter_view, VkSampler prefilter_sampler,
                VkBuffer params_buffer, uint32_t prefilter_mip_levels,
                uint32_t width, uint32_t height,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& inv_view_proj,
                const math::Vector3f& camera_pos,
                const PostProcessParams& pp);

    // 最终合成图（全分辨率），供后端拷回 HDR 颜色。仅在 valid() 且一次
    // render() 成功之后有效。
    VkImage composite_image() const { return out_image_; }
    int composite_width() const { return out_w_; }
    int composite_height() const { return out_h_; }

private:
    bool create_targets(uint32_t width, uint32_t height, uint32_t trace_width,
                        uint32_t trace_height);
    void destroy_targets();
    bool ensure_targets(uint32_t width, uint32_t height, uint32_t trace_width,
                        uint32_t trace_height);
    bool create_image(VkImage& image, VmaAllocation& alloc, VkImageView& view,
                      uint32_t width, uint32_t height, VkImageUsageFlags extra_usage);
    // 一次性命令：在当前布局与目标布局间转置图像（用内部命令池 + 队列提交 + 栅栏等待）。
    void transition_image(VkImage image, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkImageAspectFlags aspect);
    // 帧内布局转置（录在调用方命令缓冲上）。
    void cmd_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                              VkImageLayout new_layout, VkAccessFlags src_access,
                              VkAccessFlags dst_access) const;

    VulkanDevice* device_ = nullptr;
    bool valid_ = false;

    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline trace_pipeline_ = VK_NULL_HANDLE;      // 模式 0：半分辨率光线查询
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;  // 模式 1：全分辨率合成
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet trace_set_ = VK_NULL_HANDLE;      // 槽 0：uOut = trace 结果图
    VkDescriptorSet composite_set_ = VK_NULL_HANDLE;  // 槽 1：uOut = 全分辨率输出图

    VulkanBuffer* ubo_ = nullptr;          // uniform 缓冲（camera + 参数）

    VkCommandPool one_shot_pool_ = VK_NULL_HANDLE;

    // trace 结果图（trace 分辨率，RGBA16F：RGB=反射色 A=混合系数；
    // STORAGE + SAMPLED，供 composite 采样）
    VkImage refl_image_ = VK_NULL_HANDLE;
    VmaAllocation refl_alloc_ = nullptr;
    VkImageView refl_view_ = VK_NULL_HANDLE;
    int refl_w_ = 0;
    int refl_h_ = 0;
    // refl 图当前布局：每帧 trace 前须为 GENERAL（存储写入）。
    VkImageLayout refl_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // composite 输出图（全分辨率 RGBA16F；STORAGE + TRANSFER_SRC）
    VkImage out_image_ = VK_NULL_HANDLE;
    VmaAllocation out_alloc_ = nullptr;
    VkImageView out_view_ = VK_NULL_HANDLE;
    int out_w_ = 0;
    int out_h_ = 0;
    // out 图当前布局：composite 写入需 GENERAL，拷回需 TRANSFER_SRC，逐帧往返。
    VkImageLayout out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace gryce_engine::render
