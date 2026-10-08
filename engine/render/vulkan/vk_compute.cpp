// vk_compute.cpp — Vulkan 计算管线（GPU 剔除 / 间接绘制的计算端）
//
// 与 GL 后端同一套 RHI 契约：create_compute_shader(name, dir) / bind_storage_buffer
// / dispatch_compute 在两侧语义一致，差别只在实现：
//   GL     —— 单阶段 GL 程序 + glBindBufferBase + glDispatchCompute
//   Vulkan —— 计算管线 + 描述符集 + vkCmdDispatch（必须在 render pass 之外）
//
// SPIR-V 反射只做最小子集（OpName / OpDecorate / OpVariable），够用且零依赖：
// 计算着色器只有 SSBO，没有 UBO / 贴图 / push constant，不需要完整反射库。

#include "vk_compute.h"

#include <set>
#include <string>

#include "vk_device.h"
#include "vk_glsl_compiler.h"
#include "vk_spirv_cache.h"
#include "render/shader_source_resolver.h"
#include "render/render.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace gryce_engine::render {

namespace {

// SPIR-V 操作码（只需这五条）
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpVariable = 59;
// Decoration / StorageClass 枚举值
constexpr uint32_t kDecorationBlock = 2;
constexpr uint32_t kDecorationBinding = 33;
// 两种方言对 SSBO 的表示不同（实测 glslangValidator）：
//   -V（Vulkan 语义，shaderc 走这条）→ storage class = StorageBuffer(12)
//   -G（OpenGL 语义）             → storage class = Uniform(2) + 结构体带 Block 装饰
// 两种都要认，否则跨方言校验（例如离线跑 GL 版着色器）会一个 binding 都取不到。
constexpr uint32_t kStorageClassUniform = 2;
constexpr uint32_t kStorageClassStorageBuffer = 12;

// 从 SPIR-V 的字序列里取出字面字符串（小端，'\0' 结尾）
std::string decode_literal_string(const uint32_t* words, uint32_t count) {
    std::string out;
    out.reserve(count * 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w = words[i];
        for (uint32_t b = 0; b < 4; ++b) {
            char c = static_cast<char>((w >> (8 * b)) & 0xFFu);
            if (c == '\0') return out;
            out.push_back(c);
        }
    }
    return out;
}

bool read_spirv_file(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0 || size % 4 != 0) return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size) / 4);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<std::streamoff>(file.gcount()) == size;
}

} // namespace

std::vector<SpirvStorageBinding> reflect_storage_buffers(const std::vector<uint32_t>& spirv,
                                                          size_t* storage_var_count) {
    std::vector<SpirvStorageBinding> out;
    if (storage_var_count) *storage_var_count = 0;
    // 头部 5 个字：magic / version / generator / bound / schema
    if (spirv.size() < 6) return out;

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> binding_of;
    // OpTypePointer：result id → 被指向的类型 id。OpVariable 的 result type 是
    // 指针类型，块名挂在它指向的结构体上，必须多走一步。
    std::unordered_map<uint32_t, uint32_t> pointee_of;
    // 带 Block 装饰的结构体（OpenGL 方言的 SSBO 判据）
    std::unordered_map<uint32_t, uint32_t> struct_ids;
    std::unordered_map<uint32_t, uint32_t> block_structs;
    // (result id, result type id) —— StorageBuffer 变量
    std::vector<std::pair<uint32_t, uint32_t>> storage_vars;

    size_t i = 5;
    while (i < spirv.size()) {
        const uint32_t word = spirv[i];
        const uint32_t op = word & 0xFFFFu;
        const uint32_t len = word >> 16;
        if (len == 0 || i + len > spirv.size()) break;  // 畸形或已到末尾

        const uint32_t* ops = spirv.data() + i + 1;
        const uint32_t nops = len - 1;

        switch (op) {
            case kOpName:
                if (nops >= 2) names[ops[0]] = decode_literal_string(ops + 1, nops - 1);
                break;
            case kOpTypePointer:
                // ops[0] = result type id, ops[1] = storage class, ops[2] = pointee id
                if (nops >= 3) pointee_of[ops[0]] = ops[2];
                break;
            case kOpTypeStruct:
                if (nops >= 1) struct_ids[ops[0]] = 0;
                break;
            case kOpDecorate:
                // ops[0] = target, ops[1] = decoration, ops[2..] = 字面量
                if (nops >= 2 && ops[1] == kDecorationBlock) block_structs[ops[0]] = 0;
                if (nops >= 3 && ops[1] == kDecorationBinding) binding_of[ops[0]] = ops[2];
                break;
            case kOpVariable:
                // ops[0] = result type, ops[1] = result id, ops[2] = storage class
                if (nops >= 3) {
                    const uint32_t sc = ops[2];
                    bool is_ssbo = (sc == kStorageClassStorageBuffer);
                    if (!is_ssbo && sc == kStorageClassUniform) {
                        // OpenGL 方言：Uniform + pointee 是带 Block 装饰的结构体
                        auto pit = pointee_of.find(ops[0]);
                        if (pit != pointee_of.end() && block_structs.count(pit->second) &&
                            struct_ids.count(pit->second)) {
                            is_ssbo = true;
                        }
                    }
                    if (is_ssbo) storage_vars.emplace_back(ops[1], ops[0]);
                }
                break;
            default:
                break;
        }
        i += len;
    }

    if (storage_var_count) *storage_var_count = storage_vars.size();
    for (const auto& var : storage_vars) {
        SpirvStorageBinding b;
        // 块名的落点（对 glslangValidator -V -g 实测得到）：
        //   `buffer X { ... };`  → OpName 打在**结构体类型**上（名字 X），
        //                          变量自身的 OpName 是空串；
        //   `buffer X { ... } inst;` → 名字落在变量上（inst），但那不是块名。
        // 而 OpVariable 的 result type 还是个**指针**类型，所以查找顺序是：
        //   变量 → 指针的pointee（结构体）→ 空则放弃。
        // 取不到名字就无法与 CPU 侧 bind_storage_buffer 的名字对齐，只能跳过。
        auto nit = names.find(var.first);
        if (nit == names.end() || nit->second.empty()) {
            uint32_t type_id = var.second;
            auto pit = pointee_of.find(type_id);
            if (pit != pointee_of.end()) type_id = pit->second;
            auto tit = names.find(type_id);
            if (tit != names.end()) nit = tit;
        }
        if (nit != names.end()) b.name = nit->second;
        auto bit = binding_of.find(var.first);
        if (bit != binding_of.end()) {
            b.binding = bit->second;
        } else {
            // 没有显式 layout(binding=N) 的块无法与 CPU 侧约定对齐，跳过。
            GLOG_WARN("SPIR-V: storage buffer '{}' has no explicit binding, ignored", b.name);
            continue;
        }
        if (b.name.empty()) {
            GLOG_WARN("SPIR-V: storage buffer at binding {} has no block name, ignored", b.binding);
            continue;
        }
        out.push_back(std::move(b));
    }
    return out;
}

VulkanComputePipeline::~VulkanComputePipeline() {
    destroy();
}

bool VulkanComputePipeline::load(const std::string& name, const std::string& shader_dir) {
    destroy();
    if (!device_ || !device_->is_valid() || name.empty()) return false;

    std::string dir = resources::ResourcePath::resolve(shader_dir);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

    std::vector<uint32_t> code;
    // 1) 源码 → SPIR-V（磁盘缓存命中则直接读，否则 shaderc 首编并写盘）
    const ShaderStageSource src =
        resolve_shader_stage_source(name, shader_dir, ".comp", RenderAPI::Vulkan);
    if (src.ok()) {
        const std::string spv_path = dir + "spirv/vulkan_" + name + ".comp.spv";
        std::string err;
        if (!load_or_compile_spirv(src.code, src.path, spv_path, GlslStage::Compute, code, err)) {
            GLOG_WARN("VulkanComputePipeline: compile of '{}.comp' failed ({}); "
                      "falling back to pre-compiled SPIR-V", name, err);
            code.clear();
        }
    }
    // 2) 回退预编译产物
    if (code.empty()) {
        const std::string spv_path = dir + "spirv/vulkan_" + name + ".comp.spv";
        if (!read_spirv_file(spv_path, code)) {
            GLOG_WARN("VulkanComputePipeline: no SPIR-V for '{}' ({}), compute disabled",
                      name, spv_path);
            return false;
        }
    }
    return create_pipeline(code);
}

bool VulkanComputePipeline::create_pipeline(const std::vector<uint32_t>& code) {
    VkDevice dev = device_->device();

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code.size() * sizeof(uint32_t);
    module_info.pCode = code.data();
    if (vkCreateShaderModule(dev, &module_info, nullptr, &module_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkCreateShaderModule failed");
        return false;
    }

    // 反射：块名 → binding
    size_t storage_var_count = 0;
    const auto reflected = reflect_storage_buffers(code, &storage_var_count);
    for (const auto& b : reflected) {
        binding_by_name_[b.name] = b.binding;
    }
    bindings_.clear();
    for (const auto& kv : binding_by_name_) bindings_.push_back(kv.second);
    std::sort(bindings_.begin(), bindings_.end());
    bindings_.erase(std::unique(bindings_.begin(), bindings_.end()), bindings_.end());

    if (storage_var_count > 0 && bindings_.empty()) {
        // 着色器确实有 SSBO，但一个块名都没解析出来 → 十有八九是 SPIR-V 没带
        // 调试名（OpName）。此时若继续建管线，会得到"没有描述符的布局"，
        // 着色器访问 SSBO 属未定义行为。宁可整体放弃，让上层回退 CPU 路径。
        GLOG_ERROR("VulkanComputePipeline: {} SSBO block(s) found but no block name resolved "
                   "(SPIR-V without debug names?); compute disabled",
                   storage_var_count);
        return false;
    }
    if (bindings_.empty()) {
        GLOG_WARN("VulkanComputePipeline: no storage buffer bindings found in shader");
    }

    // 占位缓冲：未被显式绑定的槽位也要有合法描述符
    dummy_.init(device_, 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    layout_bindings.reserve(bindings_.size());
    for (uint32_t b : bindings_) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        layout_bindings.push_back(lb);
    }

    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = static_cast<uint32_t>(layout_bindings.size());
    set_info.pBindings = layout_bindings.empty() ? nullptr : layout_bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    // 计算着色器的参数（实例数 / 视锥面）走 SSBO，不用 push constant：
    // GL 没有 push constant，两侧共用一份 cull_instances 语义更省事。
    layout_info.pushConstantRangeCount = 0;
    if (vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkCreatePipelineLayout failed");
        return false;
    }

    const uint32_t per_set = std::max<uint32_t>(1u, static_cast<uint32_t>(bindings_.size()));
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = per_set * k_slots;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = k_slots;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkCreateDescriptorPool failed");
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(k_slots, set_layout_);
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = k_slots;
    alloc_info.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(dev, &alloc_info, sets_.data()) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkAllocateDescriptorSets failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module_;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipe_info{};
    pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe_info.stage = stage;
    pipe_info.layout = layout_;
    if (vkCreateComputePipelines(dev, device_->pipeline_cache(), 1, &pipe_info, nullptr,
                                 &pipeline_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanComputePipeline: vkCreateComputePipelines failed");
        return false;
    }

    dirty_ = true;
    return true;
}

void VulkanComputePipeline::set_storage_buffer(const char* name, VkBuffer buffer) {
    if (!name || !valid()) return;
    auto it = binding_by_name_.find(name);
    if (it == binding_by_name_.end()) {
        // 同 GL 侧：绑定失败 = 剔除静默失效，必须显眼。见 gl_backend.cpp 的说明。
        static thread_local std::set<std::string> warned;
        if (warned.insert(std::string(name)).second) {
            GLOG_ERROR("VulkanComputePipeline: no storage buffer block named '{}' in compute "
                       "shader (block names are case-sensitive). The compute pass will read an "
                       "unbound descriptor — culling silently does nothing. Further occurrences "
                       "of this name will be silent.", name);
        }
        return;
    }
    auto cur = bound_.find(it->second);
    if (cur != bound_.end() && cur->second == buffer) return;  // 未变化
    bound_[it->second] = buffer;
    dirty_ = true;
}

void VulkanComputePipeline::update_descriptors() {
    if (!dirty_ || pool_ == VK_NULL_HANDLE) return;

    std::vector<VkDescriptorBufferInfo> infos;
    infos.reserve(bindings_.size());
    for (uint32_t b : bindings_) {
        auto it = bound_.find(b);
        VkBuffer buf = VK_NULL_HANDLE;
        if (it != bound_.end() && it->second != VK_NULL_HANDLE) {
            buf = it->second;
        } else if (dummy_.buffer() != VK_NULL_HANDLE) {
            buf = dummy_.buffer();
        }
        if (buf == VK_NULL_HANDLE) continue;
        VkDescriptorBufferInfo info{};
        info.buffer = buf;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;
        infos.push_back(info);
    }

    // 指针必须在 infos 填充完后再取（避免 vector 扩容导致悬垂）
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets_[slot_];
        w.dstBinding = (i < bindings_.size()) ? bindings_[i] : 0;
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &infos[i];
        writes.push_back(w);
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    dirty_ = false;
}

void VulkanComputePipeline::dispatch(VkCommandBuffer cmd, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!valid() || cmd == VK_NULL_HANDLE) return;
    // 必须先于 dispatch 把描述符写进"本帧槽位"：轮转过槽位后 dirty_ 恒为 true，
    // 因此每帧都会重写一次，不会碰到前几帧仍在使用的那套。
    update_descriptors();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[slot_],
                            0, nullptr);
    vkCmdDispatch(cmd, gx, gy, gz);

    // compute 写 SSBO（可见实例矩阵）与间接绘制参数，随后要被顶点着色器读取
    // 与间接绘制消费：
    //   SHADER_WRITE → SHADER_READ / VERTEX_ATTRIBUTE_READ / INDIRECT_COMMAND_READ
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                            VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanComputePipeline::on_begin_frame() {
    slot_ = (slot_ + 1) % k_slots;
    dirty_ = true;
}

void VulkanComputePipeline::destroy() {
    if (!device_ || !device_->is_valid()) {
        module_ = VK_NULL_HANDLE;
        set_layout_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        sets_.fill(VK_NULL_HANDLE);
        return;
    }
    VkDevice dev = device_->device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    if (module_) vkDestroyShaderModule(dev, module_, nullptr);
    dummy_.shutdown();

    pipeline_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    module_ = VK_NULL_HANDLE;
    sets_.fill(VK_NULL_HANDLE);
    binding_by_name_.clear();
    bindings_.clear();
    bound_.clear();
    dirty_ = true;
}

} // namespace gryce_engine::render
