#pragma once

// ---------------------------------------------------------------------------
// VkRTTransmissionPass — Vulkan RT 折射 / 透射（P1）
//
// 单次派发的计算通道：自相机对每个像素打一条主视线（ray query），命中后按
// instanceCustomIndex 从 MatParams 读出该表面自己的 transmission / ior /
// attenuation_*，按 Snell 定律折射，追踪折射光线取"透过来的东西"，再按
// Beer-Lambert 吸收衰减，最后混合回 HDR。
//
// 为什么自己打主视线、而不是从深度缓冲重建表面：
//   前向管线没有 G-buffer，屏幕空间拿不到"这个像素属于哪个实例、材质是什么"。
//   主视线命中后 rayQueryGetIntersectionInstanceCustomIndexEXT 直接给出实例下标，
//   材质随之可得 —— 无需给前向 pass 加 MRT / G-buffer，也无需改前向着色器。
//   这套"先打主视线、再取材质"的结构也正是 P2 路径追踪要复用的入口。
//
// 当前近似（P1 范围，随 P2 收敛）：
//   - 单次折射：入射一次折射后直接取背景，不做"进入 + 内部传播 + 出射"两次折射；
//     厚壁按 thickness 吸收，thickness=0 时按折射光线实际命中距离吸收。
//   - 折射命中屏幕内取 HDR 双线性、屏幕外取环境（与反射通道同口径）。
//   - transmission=0 的像素整帧 passthrough，画面与现状零差异。
// ---------------------------------------------------------------------------

#include <vulkan/vulkan.h>

#include <cstdint>

#include "math/math.h"
#include "render/shader.h"
#include "render/vulkan/vk_buffer.h"
#include "render/vulkan/vk_ray_query_pass.h"  // 复用 RayQueryUniforms（std140 布局一致）

namespace gryce_engine::render {

class VulkanDevice;
class VulkanBuffer;

class VkRTTransmissionPass {
public:
    VkRTTransmissionPass() = default;
    ~VkRTTransmissionPass() { shutdown(); }

    VkRTTransmissionPass(const VkRTTransmissionPass&) = delete;
    VkRTTransmissionPass& operator=(const VkRTTransmissionPass&) = delete;

    // 编译着色器、建计算管线 / 描述符集 / uniform 缓冲。失败返回 false
    // （shaderc 不可用或特性缺失），上层应整体跳过本通道。
    bool init(VulkanDevice* device);
    void shutdown();

    bool valid() const { return valid_; }

    // 跑一次折射。最终结果写入内部全分辨率合成图（composite_image()），
    // 由后端拷回 HDR 颜色。tlas 为空时调用方应先返回（无几何无折射）。
    void render(VkCommandBuffer cmd,
                VkAccelerationStructureKHR tlas,
                VkImageView depth_view,
                VkImageView lit_view,
                VkImageView normal_view,
                VkImageView env_view, VkSampler env_sampler,
                VkImageView prefilter_view, VkSampler prefilter_sampler,
                VkBuffer params_buffer, uint32_t prefilter_mip_levels,
                uint32_t width, uint32_t height,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& inv_view_proj,
                const math::Vector3f& camera_pos,
                const PostProcessParams& pp);

    VkImage composite_image() const { return out_image_; }
    int composite_width() const { return out_w_; }
    int composite_height() const { return out_h_; }

private:
    bool ensure_targets(uint32_t width, uint32_t height);
    void destroy_targets();
    bool create_image(VkImage& image, VmaAllocation& alloc, VkImageView& view,
                      uint32_t width, uint32_t height);
    // 帧内布局转置（录在调用方命令缓冲上，无需一次性命令池）。
    void cmd_transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                              VkImageLayout new_layout, VkAccessFlags src_access,
                              VkAccessFlags dst_access) const;

    VulkanDevice* device_ = nullptr;
    bool valid_ = false;

    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;

    VulkanBuffer* ubo_ = nullptr;
    // 环境缺失时的兜底采样器：组合图像采样器描述符不能留空（空视图/空采样器会让
    // 驱动在 vkCmdDispatch 内崩），着色器按 envParams.z == 0 门控，不会真采样。
    VkSampler fallback_sampler_ = VK_NULL_HANDLE;

    VkImage out_image_ = VK_NULL_HANDLE;
    VmaAllocation out_alloc_ = nullptr;
    VkImageView out_view_ = VK_NULL_HANDLE;
    int out_w_ = 0;
    int out_h_ = 0;
    VkImageLayout out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace gryce_engine::render
