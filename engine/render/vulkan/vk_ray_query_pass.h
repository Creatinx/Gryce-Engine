#pragma once

// ---------------------------------------------------------------------------
// vk_ray_query_pass — Vulkan 硬件光追反射（P2.2，ray query）
//
// 自包含的光线查询（ray query）计算通道：用 VK_KHR_ray_query 在 TLAS 上做
// 反射射线求交，把反射合成进一张内部 RGBA16F 存储图（再由后端拷回 HDR 颜色）。
//
// 设计要点：
//   - 仅 GRYCE_HAS_VULKAN 下编译；调用方（VulkanBackend）必须已确认
//     supports_ray_tracing() 为真，否则不会实例化本类。
//   - 着色器内联为 C++ 字符串，经 compile_glsl_to_spirv 编译（shaderc 缺失时
//     优雅失败，本通道置为无效，上层回退 SSR，行为与现状零差异）。
//   - 射线查询本身在着色器内完成（GLSL rayQueryEXT），不需要 vkCmdTraceRays 等
//     主机端 KHR 命令，因此无需解析额外 PFN。
//   - 反射结果落在一张自管的存储图（STORAGE + TRANSFER_SRC），避免依赖引擎
//     HDR 颜色图是否带 STORAGE usage（当前不带）。
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
    math::Matrix4f inv_view_proj;          // 重建世界坐标
    math::Matrix4f view_proj;              // 命中点重投影回屏幕
    math::Vector4f cam_pos_max_dist;       // xyz=相机世界坐标, w=最大追踪距离
    math::Vector4f resolution_strength;    // xy=分辨率, z=反射强度
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

    // 用 TLAS 跑一次光线查询反射，结果写入内部反射图。
    // tlas 为空（场景无几何实例）时直接整帧 passthrough，不产生反射。
    // depth_view / lit_view 为 G-buffer 深度与 HDR 颜色的图像视图（采样用）。
    void render(VkCommandBuffer cmd,
                VkAccelerationStructureKHR tlas,
                VkImageView depth_view,
                VkImageView lit_view,
                uint32_t width, uint32_t height,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& inv_view_proj,
                const math::Vector3f& camera_pos,
                const PostProcessParams& pp);

    // 内部反射图（合成结果），供后端拷回 HDR 颜色。仅在 valid() 且一次
    // render() 成功之后有效。
    VkImage reflection_image() const { return refl_image_; }
    VkImageView reflection_view() const { return refl_view_; }
    int reflection_width() const { return refl_w_; }
    int reflection_height() const { return refl_h_; }

private:
    bool create_reflection_target(uint32_t width, uint32_t height);
    void destroy_reflection_target();
    bool ensure_reflection_target(uint32_t width, uint32_t height);
    // 一次性命令：在当前布局与目标布局间转置图像（用内部命令池 + 队列提交 + 栅栏等待）。
    void transition_image(VkImage image, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkImageAspectFlags aspect);

    VulkanDevice* device_ = nullptr;
    bool valid_ = false;

    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;

    VulkanBuffer* ubo_ = nullptr;          // uniform 缓冲（camera + 参数）

    VkCommandPool one_shot_pool_ = VK_NULL_HANDLE;

    // 内部反射图（STORAGE + TRANSFER_SRC，RGBA16F）
    VkImage refl_image_ = VK_NULL_HANDLE;
    VmaAllocation refl_alloc_ = nullptr;
    VkImageView refl_view_ = VK_NULL_HANDLE;
    int refl_w_ = 0;
    int refl_h_ = 0;
    VkFormat refl_format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    // 反射图当前布局：每帧 render() 先转回 GENERAL（存储写入），派发后再转
    // TRANSFER_SRC 交给后端拷回。不跟踪的话第二帧起 old layout 会对不上。
    VkImageLayout refl_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    bool tlas_bound_ = false;
};

} // namespace gryce_engine::render
