#pragma once

// vk_rt_pathtrace_pass — Vulkan RT 路径追踪 GI（P2）
//
// 单派发计算通道：主视线 ray query 取命中点后，按 bounce 循环做 cosine 加权
// 半球采样路径追踪：
//   - 命中面辐照度优先取屏幕投影的 HDR 颜色（带深度遮挡校验），投影出屏
//     或被遮挡时回退环境立方体贴图；
//   - 间接光经弹射按 albedo*(1-metallic) 调制，GI 带上命中面颜色；
//   - 金属度接近 1 的表面不产生漫反射间接光（能量守恒）。
// 输出 = lit + 间接光贡献，由后端 blit 回 HDR。
//
// 与 P1 折射通道共用 TLAS、16 float/组 MatParams SSBO 与深度/法线预通道；
// 关闭或通道无效（shaderc 缺失 / GLSL 编译失败）时整段跳过，画面零差异。
// 说明：弹射命中点法线用预通道投影近似，多弹射为有偏估计；正式的无偏
// 多弹射（命中组着色器取三角真法线）属后续 RT 管线（SBT）阶段。

#include <vulkan/vulkan.h>

#include "math/math.h"
#include "render/shader.h"
#include "render/vulkan/vk_ray_query_pass.h"  // RayQueryUniforms
#include "render/vulkan/vk_device.h"
#include "render/vulkan/vk_buffer.h"

namespace gryce_engine::render {

class VkRTPathTracePass {
public:
    bool init(VulkanDevice* device);
    void shutdown();
    bool valid() const { return valid_; }

    // cmd 须处于 recording 状态；depth/lit 为全分辨率预通道视图（lit 含
    // 反射合成前的基色）。color_aspect 仅用于输出图屏障（恒 COLOR）。
    void render(VkCommandBuffer cmd, VkAccelerationStructureKHR tlas,
                VkImageView depth_view, VkImageView lit_view,
                VkImageView normal_view,
                VkImageView env_view, VkSampler env_sampler,
                VkBuffer params_buffer,
                uint32_t width, uint32_t height,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& inv_view_proj,
                const math::Vector3f& camera_pos,
                const PostProcessParams& pp);

    VkImage composite_image() const { return out_image_; }
    VkImageLayout composite_layout() const { return out_layout_; }

private:
    bool create_image(VkImage& image, VmaAllocation& alloc, VkImageView& view,
                      uint32_t width, uint32_t height);
    void destroy_targets();
    bool ensure_targets(uint32_t width, uint32_t height);
    void cmd_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                              VkImageLayout new_layout, VkAccessFlags src_access,
                              VkAccessFlags dst_access) const;

    VulkanDevice* device_ = nullptr;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VulkanBuffer* ubo_ = nullptr;
    VkSampler fallback_sampler_ = VK_NULL_HANDLE;

    VkImage out_image_ = VK_NULL_HANDLE;
    VmaAllocation out_alloc_ = nullptr;
    VkImageView out_view_ = VK_NULL_HANDLE;
    int out_w_ = 0;
    int out_h_ = 0;
    VkImageLayout out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool valid_ = false;
};

} // namespace gryce_engine::render
