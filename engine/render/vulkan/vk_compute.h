#pragma once

#include "vk_buffer.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gryce_engine::render {

class VulkanDevice;

// SPIR-V 反射结果：一个 SSBO（storage class = StorageBuffer）的块名与 binding。
struct SpirvStorageBinding {
    std::string name;    // GLSL 块名（不带实例名，与 GL 的块名约定一致）
    uint32_t binding = 0;
};

// 从 SPIR-V 里提取所有 SSBO 的 块名 → binding。
// 只解析需要的三条指令（OpName / OpDecorate / OpVariable），不引入 spirv-reflect
// 依赖。binding 号取自着色器里的 layout(binding = N)，保证 CPU 与 GPU 约定一致。
//
// 依赖前提：SPIR-V 必须带调试名（OpName）——glslang/shaderc 默认不生成，
// glsl_compiler 对 Compute 阶段强制打开 generate_debug_info。
// debug_names 为 false 时通常只能拿到 binding、拿不到块名。
// storage_var_count 返回着色器里 StorageBuffer 变量的总数（即使名字解析失败），
// 供调用方判断"是本来就没有 SSBO"还是"名字丢了"，前者应放弃加载而不是静默降级。
std::vector<SpirvStorageBinding> reflect_storage_buffers(const std::vector<uint32_t>& spirv,
                                                          size_t* storage_var_count = nullptr);

// ---------------------------------------------------------------------------
// VulkanComputePipeline — 单条计算管线 + 一套 SSBO 描述符
// ---------------------------------------------------------------------------
// GPU 剔除（cull_instances.comp）等计算 pass 的宿主。与图形管线分开实现：
// Vulkan 的计算管线不需要 render pass / vertex input / 混合状态，挂在已经很庞大
// 的 VulkanShader 上只会把两条互不相关的状态机纠缠在一起。
//
// 描述符集按在途帧数开 k_slots 套并每帧轮转，避免"更新描述符集"撞上 GPU 仍在
// 使用的那一套（Vulkan 不允许更新正在被命令缓冲引用的描述符集，除非
// UPDATE_AFTER_BIND）。SSBO 本身的多槽位轮转见 VulkanStorageBuffer。
class VulkanComputePipeline {
public:
    explicit VulkanComputePipeline(VulkanDevice* device) : device_(device) {}
    ~VulkanComputePipeline();

    VulkanComputePipeline(const VulkanComputePipeline&) = delete;
    VulkanComputePipeline& operator=(const VulkanComputePipeline&) = delete;

    // 加载并编译 {shader_dir}/vulkan_{name}.comp（缺失回退 {name}.comp），
    // 找不到源码时回退预编译 {shader_dir}/spirv/vulkan_{name}.comp.spv。
    bool load(const std::string& name, const std::string& shader_dir);
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    // 按 GLSL 块名绑定 SSBO（binding 号由反射得出）。offset 必须传
    // VulkanStorageBuffer::frame_offset()：CPU 写（IBuffer::update）与
    // vkCmdDrawIndirect 都落在当前帧槽位，描述符不同步就会读到上一周期的
    // 数据（表现为隔帧渲染/实例消失）。
    void set_storage_buffer(const char* name, VkBuffer buffer, VkDeviceSize offset = 0);
    // 绑定管线 + 描述符集 → 派发 → 插入 compute→draw 的内存屏障
    void dispatch(VkCommandBuffer cmd, uint32_t gx, uint32_t gy, uint32_t gz);
    // 每帧开始：轮转描述符集槽位（此时 frame fence 已等过，上一周期已空闲）
    void on_begin_frame();

private:
    bool create_pipeline(const std::vector<uint32_t>& code);
    void update_descriptors();
    void destroy();

    // 与在途帧数一致（VulkanSwapchain 为 3）
    static constexpr uint32_t k_slots = 3;

    VulkanDevice* device_ = nullptr;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, k_slots> sets_{};
    uint32_t slot_ = 0;
    bool dirty_ = true;

    std::unordered_map<std::string, uint32_t> binding_by_name_;
    std::vector<uint32_t> bindings_;                // 去重、升序
    // binding → (缓冲, 帧槽字节偏移)。偏移随帧轮转变化，去重时必须一起比较，
    // 否则"缓冲没变但槽位转了"会被误判为未变化而跳过描述符更新。
    std::unordered_map<uint32_t, std::pair<VkBuffer, VkDeviceSize>> bound_;
    // 未绑定槽位的占位缓冲：Vulkan 要求描述符集里被着色器引用的每个 binding
    // 都必须写过有效缓冲，否则是未定义行为。
    VulkanBuffer dummy_;
};

} // namespace gryce_engine::render
