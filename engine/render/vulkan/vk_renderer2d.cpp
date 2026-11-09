#include "vk_renderer2d.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "assets/texture_data.h"
#include "render/render_context.h"
#include "render/render.h"
#include "render/round_rect_geometry.h"
#include "render/shader_source_resolver.h"
#include "render/vulkan/vk_backend.h"
#include "render/vulkan/vk_device.h"
#include "render/vulkan/vk_swapchain.h"
#include "render/vulkan/vk_texture.h"
#include "render/vulkan/vk_framebuffer.h"
#include "render/vulkan/vk_spirv_cache.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

static std::string get_system_font_dir() {
#ifdef _WIN32
    char win_dir[MAX_PATH] = {};
    UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir = win_dir;
        if (!dir.empty() && dir.back() != '\\') {
            dir += '\\';
        }
        return dir + "Fonts\\";
    }
    GLOG_WARN("get_system_font_dir: GetWindowsDirectoryA failed, falling back to hardcoded path");
#endif
    return "C:\\Windows\\Fonts\\";
}

bool load_spirv_file(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        GLOG_ERROR("VulkanRenderer2D: failed to open SPIR-V file '{}'", path);
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size % 4 != 0) {
        GLOG_ERROR("VulkanRenderer2D: SPIR-V file size not aligned to 4 bytes");
        return false;
    }
    out.resize(static_cast<size_t>(size) / 4);
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to read SPIR-V file");
        return false;
    }
    return true;
}

VkShaderModule create_shader_module(VkDevice dev, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &info, nullptr, &module);
    return module;
}

// 单阶段着色器加载：共享的 SPIR-V 缓存（内存 → 磁盘 → shaderc 首编并落盘），
// 源码走两级解析以支持项目覆盖 + core 兜底；源码缺失/编译失败才回退
// `spirv/vulkan_{base}.{vert|frag}.spv` 预编译产物。
// from_source 置真表示本次由源码得到 SPIR-V（可能来自缓存）。
VkShaderModule load_2d_stage(VkDevice dev, const std::string& base, GlslStage stage,
                             const std::string& spirv_dir, bool& from_source) {
    from_source = false;
    const char* ext = stage == GlslStage::Vertex ? ".vert" : ".frag";
    const char* short_ext = stage == GlslStage::Vertex ? "vert" : "frag";
    const std::string spv = spirv_dir + "vulkan_" + base + "." + short_ext + ".spv";

    ShaderStageSource src = resolve_shader_stage_source(base, "res:/shaders", ext,
                                                        RenderAPI::Vulkan);
    if (src.ok()) {
        std::string err;
        std::vector<uint32_t> code;
        SpirvCacheSource from = SpirvCacheSource::None;
        if (load_or_compile_spirv(src.code, src.path, spv, stage, code, err, &from)) {
            from_source = true;
            if (from == SpirvCacheSource::None) {
                GLOG_INFO("VulkanRenderer2D: 2D shader '{}' first-run compiled and cached", base);
            }
            return create_shader_module(dev, code);
        }
        GLOG_WARN("VulkanRenderer2D: compile of '{}' failed ({}); "
                  "falling back to pre-compiled SPIR-V", base, err);
    }

    std::vector<uint32_t> code;
    if (!load_spirv_file(spv, code)) {
        return VK_NULL_HANDLE;
    }
    return create_shader_module(dev, code);
}

// 受光照 sprite UBO 中单个光源数据，与 vulkan_2d_lit.frag 的 std140 布局一致。
// std140 中 vec3 必须从 16 字节边界开始，因此 dir 后需要 8 字节填充。
struct alignas(16) LightData {
    int type;                          // 0
    float pad0;                        // 4
    math::Vector2f pos;                // 8
    math::Vector2f dir;                // 16
    float pad_before_color[2];         // 24
    float color[3];                    // 32
    float pad1;                        // 44
    float intensity;                   // 48
    float radius;                      // 52
    float range;                       // 56
    float spot_angle;                  // 60
    float spot_softness;               // 64
};
static_assert(sizeof(LightData) == 80, "LightData size mismatch");

// 受光照 sprite UBO，与 vulkan_2d_lit.frag 的 std140 布局一致
struct alignas(16) LightUBO {
    math::Vector3f ambient;            // 0-11
    int light_count;                   // 12-15
    int use_shadow_map;                // 16-19
    int shadow_light_index;            // 20-23
    float pad0[2];                     // 24-31
    math::Matrix4f light_space_matrix; // 32-95
    LightData lights[32];              // 96-2655
};
static_assert(sizeof(LightUBO) == 2656, "LightUBO size mismatch");

// 全屏后处理顶点
struct FSVertex {
    float x, y, u, v;
};

} // namespace

VulkanRenderer2D::VulkanRenderer2D() = default;

VulkanRenderer2D::~VulkanRenderer2D() {
    shutdown();
}

bool VulkanRenderer2D::context_alive() const {
    return ctx_ && ctx_lifetime_ && ctx_lifetime_->alive.load();
}

void VulkanRenderer2D::init(RenderContext* ctx) {
    if (initialized_ || !ctx) return;
    ctx_ = ctx;
    ctx_lifetime_ = ctx->lifetime();

    vk_backend_ = dynamic_cast<VulkanBackend*>(ctx->backend());
    if (!vk_backend_) {
        GLOG_ERROR("VulkanRenderer2D: backend is not VulkanBackend");
        return;
    }
    vk_device_ = vk_backend_->device();
    vk_swapchain_ = vk_backend_->swapchain();

    if (!create_shader_modules() ||
        !create_descriptor_layouts() ||
        !create_pipeline_layouts() ||
        !create_swapchain_pipelines() ||
        !create_descriptor_sets() ||
        !create_vertex_buffers() ||
        !create_uniform_buffers() ||
        !create_fallback_textures() ||
        !create_shadow_map()) {
        shutdown();
        return;
    }

    // 字体查找顺序与 GL 版 Renderer2D 保持一致（Cousine 优先 = 与 imgui 轨同款）
    {
        const std::string root = resources::Project::instance().root();
        const std::string candidates[] = {
            resources::ResourcePath::resolve("res:/fonts/Cousine-Regular.ttf"),
            resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf"),
            root + "/third_party/imgui/misc/fonts/Cousine-Regular.ttf",
            root + "/third_party/imgui/misc/fonts/Roboto-Medium.ttf",
        };
        for (const std::string& path : candidates) {
            if (path.empty() || !std::filesystem::exists(path)) continue;
            if (font_atlas_.init(ctx_, path, 32.0f)) {
                using_fallback_font_ = false;
                GLOG_INFO("VulkanRenderer2D: loaded bundled font '{}'", path);
                break;
            }
        }
    }

    // 内置字体失败时尝试系统字体
    if (!font_atlas_.texture()) {
        std::string font_dir = get_system_font_dir();
        const char* font_names[] = {
            "arial.ttf",
            "segoeui.ttf",
            "tahoma.ttf",
            "calibri.ttf",
            "verdana.ttf",
            "times.ttf",
            "msyh.ttc",
            "simhei.ttf",
            nullptr
        };

        for (int i = 0; font_names[i]; ++i) {
            std::string font_path = font_dir + font_names[i];
            if (font_atlas_.init(ctx_, font_path, 32.0f)) {
                using_fallback_font_ = false;
                GLOG_INFO("VulkanRenderer2D: loaded system font '{}'", font_path);
                break;
            }
        }
    }

    if (!font_atlas_.texture()) {
        GLOG_WARN("VulkanRenderer2D: no system/bundled font loaded, creating fallback atlas");
        if (font_atlas_.create_fallback_atlas(ctx_, 32.0f)) {
            using_fallback_font_ = true;
            GLOG_INFO("VulkanRenderer2D: fallback atlas created");
        } else {
            GLOG_ERROR("VulkanRenderer2D: failed to create fallback font atlas, text rendering disabled");
        }
    }

    if (font_atlas_.texture()) {
        font_atlas_.texture()->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    vertices_.reserve(4096);
    text_vertices_.reserve(512);
    sprite_vertices_.reserve(1024);
    lit_batches_.reserve(16);
    shadow_caster_vertices_.reserve(512);
    initialized_ = true;
    GLOG_INFO("VulkanRenderer2D initialized");
}

void VulkanRenderer2D::shutdown() {
    if (!initialized_) return;

    // 先提交并等待渲染线程执行完所有已排队命令（这些命令大多捕获了 this），
    // 确保本对象销毁后不会有残留命令在渲染线程访问悬垂的 this（对齐 GL 侧）。
    // 仅在渲染线程运行中执行；暂停状态下 cmd_buffer_ 可能已为空指针。
    if (context_alive() && ctx_->is_running()) {
        ctx_->present();
        ctx_->wait_for_idle();
    }

    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }

    destroy_bloom_targets();
    destroy_ui_ssaa_target();
    destroy_shadow_map();

    if (context_alive()) {
        if (fallback_albedo_tex_.is_valid()) ctx_->destroy_texture(fallback_albedo_tex_);
        if (fallback_normal_tex_.is_valid()) ctx_->destroy_texture(fallback_normal_tex_);
    }
    fallback_albedo_ = nullptr;
    fallback_normal_ = nullptr;

    if (context_alive() && font_atlas_.texture()) {
        font_atlas_.destroy(ctx_);
    }

    for (auto& buf : vertex_buffer_rect_) buf.shutdown();
    for (auto& buf : vertex_buffer_text_) buf.shutdown();
    for (auto& buf : vertex_buffer_sprite_) buf.shutdown();
    for (auto& buf : vertex_buffer_lit_sprite_) buf.shutdown();
    for (auto& buf : vertex_buffer_shadow_caster_) buf.shutdown();
    for (auto& buf : fs_vertex_buffer_) buf.shutdown();
    for (auto& buf : light_ubo_) buf.shutdown();
    retired_buffers_.clear();

    if (dev != VK_NULL_HANDLE) {
        if (pipeline_rect_) vkDestroyPipeline(dev, pipeline_rect_, nullptr);
        if (pipeline_text_) vkDestroyPipeline(dev, pipeline_text_, nullptr);
        if (pipeline_sprite_) vkDestroyPipeline(dev, pipeline_sprite_, nullptr);
        if (pipeline_sprite_additive_) vkDestroyPipeline(dev, pipeline_sprite_additive_, nullptr);
        if (pipeline_lit_sprite_) vkDestroyPipeline(dev, pipeline_lit_sprite_, nullptr);
        if (pipeline_rect_scene_) vkDestroyPipeline(dev, pipeline_rect_scene_, nullptr);
        if (pipeline_text_scene_) vkDestroyPipeline(dev, pipeline_text_scene_, nullptr);
        if (pipeline_sprite_scene_) vkDestroyPipeline(dev, pipeline_sprite_scene_, nullptr);
        if (pipeline_sprite_scene_additive_) vkDestroyPipeline(dev, pipeline_sprite_scene_additive_, nullptr);
        if (pipeline_lit_sprite_scene_) vkDestroyPipeline(dev, pipeline_lit_sprite_scene_, nullptr);
        if (pipeline_shadow_) vkDestroyPipeline(dev, pipeline_shadow_, nullptr);
        if (pipeline_bloom_threshold_) vkDestroyPipeline(dev, pipeline_bloom_threshold_, nullptr);
        if (pipeline_bloom_blur_) vkDestroyPipeline(dev, pipeline_bloom_blur_, nullptr);
        if (pipeline_bloom_compose_) vkDestroyPipeline(dev, pipeline_bloom_compose_, nullptr);

        if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        if (lit_pipeline_layout_) vkDestroyPipelineLayout(dev, lit_pipeline_layout_, nullptr);
        if (shadow_pipeline_layout_) vkDestroyPipelineLayout(dev, shadow_pipeline_layout_, nullptr);
        if (bloom_pipeline_layout_) vkDestroyPipelineLayout(dev, bloom_pipeline_layout_, nullptr);
        if (bloom_compose_pipeline_layout_) vkDestroyPipelineLayout(dev, bloom_compose_pipeline_layout_, nullptr);

        for (auto& pool : descriptor_pools_) {
            if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (descriptor_layout_) vkDestroyDescriptorSetLayout(dev, descriptor_layout_, nullptr);
        if (lit_descriptor_layout_) vkDestroyDescriptorSetLayout(dev, lit_descriptor_layout_, nullptr);

        if (vert_module_) vkDestroyShaderModule(dev, vert_module_, nullptr);
        if (vert_lit_module_) vkDestroyShaderModule(dev, vert_lit_module_, nullptr);
        if (frag_rect_module_) vkDestroyShaderModule(dev, frag_rect_module_, nullptr);
        if (frag_text_module_) vkDestroyShaderModule(dev, frag_text_module_, nullptr);
        if (frag_sprite_module_) vkDestroyShaderModule(dev, frag_sprite_module_, nullptr);
        if (frag_lit_sprite_module_) vkDestroyShaderModule(dev, frag_lit_sprite_module_, nullptr);
        if (vert_shadow_module_) vkDestroyShaderModule(dev, vert_shadow_module_, nullptr);
        if (frag_shadow_module_) vkDestroyShaderModule(dev, frag_shadow_module_, nullptr);
        if (vert_bloom_module_) vkDestroyShaderModule(dev, vert_bloom_module_, nullptr);
        if (frag_bloom_threshold_module_) vkDestroyShaderModule(dev, frag_bloom_threshold_module_, nullptr);
        if (frag_bloom_blur_module_) vkDestroyShaderModule(dev, frag_bloom_blur_module_, nullptr);
        if (frag_bloom_compose_module_) vkDestroyShaderModule(dev, frag_bloom_compose_module_, nullptr);
    }

    initialized_ = false;
    ctx_ = nullptr;
    ctx_lifetime_.reset();
    vk_backend_ = nullptr;
    vk_device_ = nullptr;
    vk_swapchain_ = nullptr;
}

bool VulkanRenderer2D::create_shader_modules() {
    std::string resolved = resources::ResourcePath::resolve("res:/shaders/spirv");
    if (resolved.empty()) {
        GLOG_ERROR("VulkanRenderer2D: failed to resolve shader directory");
        return false;
    }
    if (resolved.back() != '/' && resolved.back() != '\\') {
        resolved += '/';
    }

    auto load_stage = [&](const std::string& base, GlslStage stage) {
        bool from_source = false;
        VkShaderModule m = load_2d_stage(vk_device_->device(), base, stage, resolved, from_source);
        return m;
    };

    // 核心 2D 四件套（矩形/文字/精灵 + 顶点）必须成功
    vert_module_ = load_stage("2d", GlslStage::Vertex);
    frag_rect_module_ = load_stage("2d_rect", GlslStage::Fragment);
    frag_text_module_ = load_stage("2d_text", GlslStage::Fragment);
    frag_sprite_module_ = load_stage("2d_sprite", GlslStage::Fragment);

    // 可选功能：2D 光照 / 硬阴影 / Bloom（任一段失败则整feature禁用）
    bool has_lit = (vert_lit_module_ = load_stage("2d_lit", GlslStage::Vertex)) != VK_NULL_HANDLE &&
                   (frag_lit_sprite_module_ = load_stage("2d_lit", GlslStage::Fragment)) != VK_NULL_HANDLE;
    bool has_shadow = (vert_shadow_module_ = load_stage("2d_shadow", GlslStage::Vertex)) != VK_NULL_HANDLE &&
                      (frag_shadow_module_ = load_stage("2d_shadow", GlslStage::Fragment)) != VK_NULL_HANDLE;
    bool has_bloom = (vert_bloom_module_ = load_stage("2d_bloom", GlslStage::Vertex)) != VK_NULL_HANDLE &&
                     (frag_bloom_threshold_module_ = load_stage("2d_bloom_threshold", GlslStage::Fragment)) != VK_NULL_HANDLE &&
                     (frag_bloom_blur_module_ = load_stage("2d_bloom_blur", GlslStage::Fragment)) != VK_NULL_HANDLE &&
                     (frag_bloom_compose_module_ = load_stage("2d_bloom_compose", GlslStage::Fragment)) != VK_NULL_HANDLE;

    if (!has_lit) {
        GLOG_WARN("VulkanRenderer2D: lit sprite shaders missing, 2D lighting disabled in Vulkan");
    }
    if (!has_shadow) {
        GLOG_WARN("VulkanRenderer2D: shadow shaders missing, 2D shadows disabled in Vulkan");
    }
    if (!has_bloom) {
        GLOG_WARN("VulkanRenderer2D: bloom shaders missing, Bloom disabled in Vulkan");
    }

    // 覆盖层超采样解析（可选）：缺少就退回 1x 直绘，不影响功能
    frag_ui_resolve_module_ = load_stage("2d_ui_resolve", GlslStage::Fragment);
    if (frag_ui_resolve_module_ == VK_NULL_HANDLE) {
        GLOG_WARN("VulkanRenderer2D: ui resolve shader missing, UI overlay SSAA disabled");
    }

    if (!vert_module_ || !frag_rect_module_ || !frag_text_module_ || !frag_sprite_module_) {
        GLOG_ERROR("VulkanRenderer2D: failed to create core 2D shader modules");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_descriptor_layouts() {
    // 普通 2D descriptor：仅一个 combined image sampler
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = &binding;

        if (vkCreateDescriptorSetLayout(vk_device_->device(), &info, nullptr, &descriptor_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create descriptor set layout");
            return false;
        }
    }

    // 受光照 sprite descriptor：albedo + normal + UBO + shadow map
    {
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 4;
        info.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(vk_device_->device(), &info, nullptr, &lit_descriptor_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create lit descriptor set layout");
            return false;
        }
    }
    return true;
}

bool VulkanRenderer2D::create_pipeline_layouts() {
    // 普通 2D pipeline layout：只有 view_proj 矩阵
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create pipeline layout");
            return false;
        }
    }

    // 受光照 sprite pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &lit_descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &lit_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create lit pipeline layout");
            return false;
        }
    }

    // 阴影 pipeline layout：无 descriptor set
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 0;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &shadow_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create shadow pipeline layout");
            return false;
        }
    }

    // Bloom threshold / blur pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Vector4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &bloom_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom pipeline layout");
            return false;
        }
    }

    // Bloom compose pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Vector4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &lit_descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &bloom_compose_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom compose pipeline layout");
            return false;
        }
    }
    return true;
}

VkPipeline VulkanRenderer2D::create_pipeline(VkShaderModule vert_module, VkShaderModule frag_module,
                                              VkPipelineLayout layout, VkRenderPass render_pass,
                                              uint32_t vertex_stride,
                                              const VkVertexInputAttributeDescription* attrs,
                                              uint32_t attr_count,
                                              bool depth_test, bool depth_write, BlendMode blend_mode,
                                              bool color_output) {
    if (!vk_device_) {
        GLOG_ERROR("VulkanRenderer2D::create_pipeline: vk_device_ is null");
        return VK_NULL_HANDLE;
    }
    VkDevice dev = vk_device_->device();
    if (dev == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D::create_pipeline: VkDevice is null");
        return VK_NULL_HANDLE;
    }
    if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D::create_pipeline: shader module is null (vert={}, frag={})",
                   reinterpret_cast<void*>(vert_module), reinterpret_cast<void*>(frag_module));
        return VK_NULL_HANDLE;
    }
    if (layout == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D::create_pipeline: pipeline layout is null");
        return VK_NULL_HANDLE;
    }
    if (render_pass == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D::create_pipeline: render pass is null");
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = vertex_stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = attr_count;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attach{};
    if (blend_mode == BlendMode::Alpha) {
        blend_attach.blendEnable = VK_TRUE;
        blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (blend_mode == BlendMode::Additive) {
        blend_attach.blendEnable = VK_TRUE;
        blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else {
        blend_attach.blendEnable = VK_FALSE;
        blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    blend_attach.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attach.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = color_output ? 1 : 0;
    blend_state.pAttachments = color_output ? &blend_attach : nullptr;

    VkDynamicState dynamics[6] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    uint32_t dynamic_count = 2;
    if (vk_backend_ && vk_backend_->supports_dynamic_state()) {
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_CULL_MODE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_FRONT_FACE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT;
    }
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = dynamic_count;
    dynamic_state.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;

    // 复用设备全局管线缓存，让驱动跳过重复的 SPIR-V → ISA 编译。
    const VkPipelineCache cache = vk_device_ ? vk_device_->pipeline_cache() : VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool VulkanRenderer2D::create_swapchain_pipelines() {
    if (!vk_swapchain_) {
        GLOG_ERROR("VulkanRenderer2D::create_swapchain_pipelines: swapchain is null");
        return false;
    }
    VkRenderPass swapchain_rp = vk_swapchain_->render_pass();
    if (swapchain_rp == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D::create_swapchain_pipelines: swapchain render pass is null");
        return false;
    }

    // Vertex2D 属性：pos(0) + color(1) + uv(2)
    VkVertexInputAttributeDescription base_attrs[3]{};
    base_attrs[0].location = 0;
    base_attrs[0].binding = 0;
    base_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[0].offset = 0;
    base_attrs[1].location = 1;
    base_attrs[1].binding = 0;
    base_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    base_attrs[1].offset = 2 * sizeof(float);
    base_attrs[2].location = 2;
    base_attrs[2].binding = 0;
    base_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[2].offset = 6 * sizeof(float);

    pipeline_rect_ = create_pipeline(vert_module_, frag_rect_module_, pipeline_layout_, swapchain_rp,
                                     sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_text_ = create_pipeline(vert_module_, frag_text_module_, pipeline_layout_, swapchain_rp,
                                     sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_sprite_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, swapchain_rp,
                                       sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_sprite_additive_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, swapchain_rp,
                                                sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Additive);

    if (vert_lit_module_ != VK_NULL_HANDLE && frag_lit_sprite_module_ != VK_NULL_HANDLE) {
        VkVertexInputAttributeDescription lit_attrs[4]{};
        lit_attrs[0].location = 0;
        lit_attrs[0].binding = 0;
        lit_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[0].offset = 0;
        lit_attrs[1].location = 1;
        lit_attrs[1].binding = 0;
        lit_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        lit_attrs[1].offset = 2 * sizeof(float);
        lit_attrs[2].location = 2;
        lit_attrs[2].binding = 0;
        lit_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[2].offset = 6 * sizeof(float);
        lit_attrs[3].location = 3;
        lit_attrs[3].binding = 0;
        lit_attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[3].offset = 8 * sizeof(float);

        pipeline_lit_sprite_ = create_pipeline(vert_lit_module_, frag_lit_sprite_module_, lit_pipeline_layout_,
                                               swapchain_rp, sizeof(LitVertex2D), lit_attrs, 4,
                                               false, false, BlendMode::Alpha);
    }

    if (pipeline_rect_ == VK_NULL_HANDLE || pipeline_text_ == VK_NULL_HANDLE ||
        pipeline_sprite_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create swapchain pipelines");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_descriptor_sets() {
    // One dynamic pool per frame-in-flight so resetting a pool never touches sets
    // that may still be executed by the GPU on an earlier frame.
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;
    // 裁剪变化会切批（每段裁剪各自成批），一帧的批次数量按界面里 clip 的
    // 数量线性增长；256 在一屏几十个滚动容器下会被打满，分配失败的批次
    // 直接丢绘制（界面上表现为整块消失）。
    constexpr uint32_t kMaxSetsPerFrame = 1024;

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = kMaxSetsPerFrame * 4; // up to 4 image bindings per set
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = kMaxSetsPerFrame;     // lit UBOs

    VkDevice dev = vk_device_->device();
    descriptor_pools_.resize(frames, VK_NULL_HANDLE);
    descriptor_pool_dirty_.assign(static_cast<std::size_t>(frames), 1);
    for (int i = 0; i < frames; ++i) {
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        pool_info.maxSets = kMaxSetsPerFrame;
        if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &descriptor_pools_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create descriptor pool {}", i);
            return false;
        }
    }
    return true;
}

VkDescriptorSet VulkanRenderer2D::allocate_descriptor_set(VkDescriptorSetLayout layout) {
    int frame_index = vk_swapchain_ ? vk_swapchain_->current_frame_index() : 0;
    if (frame_index < 0 || frame_index >= static_cast<int>(descriptor_pools_.size()) ||
        descriptor_pools_[frame_index] == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    if (frame_index < static_cast<int>(descriptor_pool_dirty_.size()) &&
        descriptor_pool_dirty_[static_cast<std::size_t>(frame_index)] != 0) {
        // 该帧槽的上一帧已经过 fence 等待（帧槽复用前必等），此时重置是安全的。
        vkResetDescriptorPool(vk_device_->device(), descriptor_pools_[frame_index], 0);
        descriptor_pool_dirty_[static_cast<std::size_t>(frame_index)] = 0;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pools_[frame_index];
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk_device_->device(), &alloc, &set) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to allocate dynamic descriptor set");
        return VK_NULL_HANDLE;
    }
    return set;
}

bool VulkanRenderer2D::grow_vertex_buffer(VulkanBuffer& buffer, VkDeviceSize& capacity,
                                          VkDeviceSize required, int frame_index, const char* what) {
    if (required <= capacity) return true;
    // 旧缓冲可能仍被本帧已录制的 draw 引用：不能立即 shutdown，移入退役
    // 列表，由 begin_frame 的渲染命令在该帧槽 fence 等待之后统一销毁。
    if (frame_index >= 0 && frame_index < static_cast<int>(retired_buffers_.size())) {
        retired_buffers_[frame_index].push_back(std::move(buffer));
    } else {
        buffer.shutdown();
    }
    capacity = required * 2;
    if (!buffer.init(vk_device_, capacity,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        GLOG_ERROR("VulkanRenderer2D: failed to resize {}", what);
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_vertex_buffers() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;

    retired_buffers_.resize(frames);
    vertex_buffer_rect_.resize(frames);
    vertex_buffer_text_.resize(frames);
    vertex_buffer_sprite_.resize(frames);
    vertex_buffer_lit_sprite_.resize(frames);
    vertex_buffer_shadow_caster_.resize(frames);
    fs_vertex_buffer_.resize(frames);

    vertex_buffer_rect_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_text_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_sprite_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_lit_sprite_capacity_.assign(frames, 4096 * sizeof(LitVertex2D));
    vertex_buffer_shadow_caster_capacity_.assign(frames, 4096 * sizeof(ShadowCasterVertex));
    fs_vertex_buffer_capacity_.assign(frames, 3 * sizeof(FSVertex));

    for (int i = 0; i < frames; ++i) {
        if (!vertex_buffer_rect_[i].init(vk_device_, vertex_buffer_rect_capacity_[i],
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_text_[i].init(vk_device_, vertex_buffer_text_capacity_[i],
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_sprite_[i].init(vk_device_, vertex_buffer_sprite_capacity_[i],
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_lit_sprite_[i].init(vk_device_, vertex_buffer_lit_sprite_capacity_[i],
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_shadow_caster_[i].init(vk_device_, vertex_buffer_shadow_caster_capacity_[i],
                                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !fs_vertex_buffer_[i].init(vk_device_, fs_vertex_buffer_capacity_[i],
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }

        // 全屏后处理 mesh 是固定的，初始化时直接上传，避免每帧重复 upload。
        FSVertex fs_verts[3] = {
            {-1.0f, -1.0f, 0.0f, 0.0f},
            { 3.0f, -1.0f, 2.0f, 0.0f},
            {-1.0f,  3.0f, 0.0f, 2.0f}
        };
        fs_vertex_buffer_[i].upload(fs_verts, sizeof(fs_verts));
    }
    return true;
}

bool VulkanRenderer2D::create_uniform_buffers() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;
    light_ubo_.resize(frames);
    for (int i = 0; i < frames; ++i) {
        if (!light_ubo_[i].init(vk_device_, sizeof(LightUBO),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer2D::create_fallback_textures() {
    fallback_albedo_tex_ = ctx_->create_texture();
    fallback_normal_tex_ = ctx_->create_texture();
    ITexture* albedo = ctx_->texture(fallback_albedo_tex_);
    ITexture* normal = ctx_->texture(fallback_normal_tex_);
    if (!albedo || !normal) return false;

    unsigned char white[] = {255, 255, 255, 255};
    unsigned char blue[] = {128, 128, 255, 255};
    if (!albedo->upload_data(white, 1, 1, 4) ||
        !normal->upload_data(blue, 1, 1, 4)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create fallback textures");
        return false;
    }
    albedo->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    albedo->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    normal->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    normal->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    fallback_albedo_ = dynamic_cast<VulkanTexture*>(albedo);
    fallback_normal_ = dynamic_cast<VulkanTexture*>(normal);
    return fallback_albedo_ && fallback_normal_;
}

bool VulkanRenderer2D::create_shadow_map() {
    if (!ctx_) return false;
    if (vert_shadow_module_ == VK_NULL_HANDLE) return true; // 无阴影 shader，不影响初始化

    shadow_map_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(shadow_map_tex_);
    if (!tex || !tex->create_depth(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow map texture");
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    shadow_map_ = dynamic_cast<VulkanTexture*>(tex);
    if (!shadow_map_) return false;

    shadow_fb_ = ctx_->create_framebuffer();
    IFramebuffer* fb = ctx_->framebuffer(shadow_fb_);
    auto* vk_fb = dynamic_cast<VulkanFramebuffer*>(fb);
    if (!vk_fb || !vk_fb->create(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow framebuffer");
        return false;
    }
    vk_fb->attach_depth_texture(tex);
    if (!vk_fb->is_complete()) {
        GLOG_ERROR("VulkanRenderer2D: shadow framebuffer incomplete");
        return false;
    }

    VkVertexInputAttributeDescription shadow_attr{};
    shadow_attr.location = 0;
    shadow_attr.binding = 0;
    shadow_attr.format = VK_FORMAT_R32G32_SFLOAT;
    shadow_attr.offset = 0;

    pipeline_shadow_ = create_pipeline(vert_shadow_module_, frag_shadow_module_, shadow_pipeline_layout_,
                                       vk_fb->render_pass(), sizeof(ShadowCasterVertex),
                                       &shadow_attr, 1, true, true, BlendMode::Opaque, false);
    if (pipeline_shadow_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow pipeline");
        return false;
    }

    return true;
}

void VulkanRenderer2D::destroy_shadow_map() {
    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (pipeline_shadow_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_shadow_, nullptr);
        pipeline_shadow_ = VK_NULL_HANDLE;
    }
    if (context_alive()) {
        if (shadow_fb_.is_valid()) ctx_->destroy_framebuffer(shadow_fb_);
        if (shadow_map_tex_.is_valid()) ctx_->destroy_texture(shadow_map_tex_);
    }
    shadow_fb_ = RHIFramebufferHandle{};
    shadow_map_tex_ = RHITextureHandle{};
    shadow_map_ = nullptr;
}

// ---------------------------------------------------------------------------
// 覆盖层超采样（SSAA）
//
// 2D 管线没有任何几何抗锯齿：矩形边、圆角、字形都是硬采样。文字靠图集里的
// 覆盖率和线性采样凑合，图形边缘则完全是台阶。这里把整个覆盖层画到 2x 尺寸
// 的离屏 RGBA8 上，再用一次全屏线性采样降回 back buffer——双线性在精确 2:1
// 上取四个 texel 的等权平均，等价于每像素 4 个样本。代价是覆盖层的填充率 ×4，
// 但覆盖层本身很轻，换来的是文字与图形边缘一起变平滑。
// ---------------------------------------------------------------------------

bool VulkanRenderer2D::create_ui_ssaa_pipeline() {
    if (pipeline_ui_resolve_ != VK_NULL_HANDLE) return true;
    if (!vk_swapchain_ || frag_ui_resolve_module_ == VK_NULL_HANDLE ||
        vert_bloom_module_ == VK_NULL_HANDLE || bloom_pipeline_layout_ == VK_NULL_HANDLE) {
        return false;
    }

    VkVertexInputAttributeDescription fs_attrs[2]{};
    fs_attrs[0].location = 0;
    fs_attrs[0].binding = 0;
    fs_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[0].offset = 0;
    fs_attrs[1].location = 1;
    fs_attrs[1].binding = 0;
    fs_attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[1].offset = 2 * sizeof(float);

    pipeline_ui_resolve_ = create_pipeline(vert_bloom_module_, frag_ui_resolve_module_,
                                           bloom_pipeline_layout_, vk_swapchain_->render_pass(),
                                           sizeof(FSVertex), fs_attrs, 2,
                                           false, false, BlendMode::Alpha);
    // 必须是 Alpha 混合：离屏目标是"透明黑"起步的，没有被 UI 覆盖到的像素
    // alpha 就是 0。按不透明写回会把整个 back buffer 刷成黑色 —— 纯 UI 预览
    // 背景全黑、3D 场景被整片盖掉，都是这一条造成的。着色器已经把 RGB 反预乘
    // 回直通色，这里正好交给固定的 SrcAlpha/OneMinusSrcAlpha。
    if (pipeline_ui_resolve_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create ui resolve pipeline");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_ui_ssaa_target_locked(int width, int height) {
    if (!ctx_ || width <= 0 || height <= 0) return false;
    destroy_ui_ssaa_target();

    ui_ssaa_texture_ = ctx_->create_texture();
    auto* vk_tex = dynamic_cast<VulkanTexture*>(ctx_->texture(ui_ssaa_texture_));
    if (!vk_tex || !vk_tex->create(TextureFormat::RGBA8, width, height, nullptr)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create ui ssaa texture");
        destroy_ui_ssaa_target();
        return false;
    }
    vk_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    vk_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    ui_ssaa_fb_ = ctx_->create_framebuffer();
    auto* vk_fb = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(ui_ssaa_fb_));
    if (!vk_fb || !vk_fb->create(width, height)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create ui ssaa framebuffer");
        destroy_ui_ssaa_target();
        return false;
    }
    vk_fb->attach_color_texture(vk_tex);
    vk_fb->set_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    if (!vk_fb->is_complete()) {
        GLOG_ERROR("VulkanRenderer2D: ui ssaa framebuffer incomplete");
        destroy_ui_ssaa_target();
        return false;
    }

    ui_ssaa_w_ = width;
    ui_ssaa_h_ = height;
    return true;
}

bool VulkanRenderer2D::ensure_ui_ssaa_target(int width, int height) {
    const int w = width * 2;
    const int h = height * 2;
    if (ui_ssaa_ready_ && ui_ssaa_w_ == w && ui_ssaa_h_ == h &&
        ui_ssaa_fb_.is_valid() && pipeline_ui_resolve_ != VK_NULL_HANDLE) {
        return true;
    }
    if (!ctx_ || !context_alive() || frag_ui_resolve_module_ == VK_NULL_HANDLE) {
        ui_ssaa_ready_ = false;
        return false;
    }

    bool ok = false;
    auto build = [this, w, h, &ok]() {
        ok = create_ui_ssaa_pipeline() && create_ui_ssaa_target_locked(w, h);
    };
    if (ctx_->is_running()) {
        ctx_->run_with_gpu_context_sync(build);
    } else {
        build();
    }
    ui_ssaa_ready_ = ok;
    return ok;
}

void VulkanRenderer2D::destroy_ui_ssaa_target() {
    if (context_alive()) {
        if (ui_ssaa_fb_.is_valid()) ctx_->destroy_framebuffer(ui_ssaa_fb_);
        if (ui_ssaa_texture_.is_valid()) ctx_->destroy_texture(ui_ssaa_texture_);
    }
    ui_ssaa_fb_ = RHIFramebufferHandle{};
    ui_ssaa_texture_ = RHITextureHandle{};
    ui_ssaa_w_ = 0;
    ui_ssaa_h_ = 0;
    ui_ssaa_ready_ = false;
}

bool VulkanRenderer2D::create_bloom_targets() {
    if (!ctx_ || screen_width_ <= 0.0f || screen_height_ <= 0.0f) {
        return false;
    }
    if (vert_bloom_module_ == VK_NULL_HANDLE) {
        return false;
    }

    destroy_bloom_pipelines();

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);

    auto create_color_fb = [&](TextureFormat fmt, RHITextureHandle& tex, RHIFramebufferHandle& fb) -> bool {
        tex = ctx_->create_texture();
        auto* vk_tex = dynamic_cast<VulkanTexture*>(ctx_->texture(tex));
        if (!vk_tex || !vk_tex->create(fmt, w, h, nullptr)) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom texture");
            return false;
        }
        vk_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        vk_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb = ctx_->create_framebuffer();
        auto* vk_fb = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(fb));
        if (!vk_fb || !vk_fb->create(w, h)) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom framebuffer");
            return false;
        }
        vk_fb->attach_color_texture(vk_tex);
        if (!vk_fb->is_complete()) {
            GLOG_ERROR("VulkanRenderer2D: bloom framebuffer incomplete");
            return false;
        }
        return true;
    };

    if (!create_color_fb(TextureFormat::RGBA16F, scene_texture_, scene_fb_) ||
        !create_color_fb(TextureFormat::RGBA8, bloom_texture_a_, bloom_fb_a_) ||
        !create_color_fb(TextureFormat::RGBA8, bloom_texture_b_, bloom_fb_b_)) {
        destroy_bloom_targets();
        return false;
    }

    // scene FBO 默认清除为透明黑
    auto* scene_fb = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(scene_fb_));
    if (scene_fb) {
        scene_fb->set_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    }

    VkRenderPass scene_rp = scene_fb->render_pass();
    VkRenderPass bloom_rp = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(bloom_fb_a_))->render_pass();
    VkRenderPass swapchain_rp = vk_swapchain_->render_pass();

    // Vertex2D 基础属性
    VkVertexInputAttributeDescription base_attrs[3]{};
    base_attrs[0].location = 0;
    base_attrs[0].binding = 0;
    base_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[0].offset = 0;
    base_attrs[1].location = 1;
    base_attrs[1].binding = 0;
    base_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    base_attrs[1].offset = 2 * sizeof(float);
    base_attrs[2].location = 2;
    base_attrs[2].binding = 0;
    base_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[2].offset = 6 * sizeof(float);

    pipeline_rect_scene_ = create_pipeline(vert_module_, frag_rect_module_, pipeline_layout_, scene_rp,
                                           sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_text_scene_ = create_pipeline(vert_module_, frag_text_module_, pipeline_layout_, scene_rp,
                                           sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_sprite_scene_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, scene_rp,
                                             sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Alpha);
    pipeline_sprite_scene_additive_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, scene_rp,
                                                      sizeof(Vertex2D), base_attrs, 3, false, false, BlendMode::Additive);

    if (vert_lit_module_ != VK_NULL_HANDLE && frag_lit_sprite_module_ != VK_NULL_HANDLE) {
        VkVertexInputAttributeDescription lit_attrs[4]{};
        lit_attrs[0].location = 0;
        lit_attrs[0].binding = 0;
        lit_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[0].offset = 0;
        lit_attrs[1].location = 1;
        lit_attrs[1].binding = 0;
        lit_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        lit_attrs[1].offset = 2 * sizeof(float);
        lit_attrs[2].location = 2;
        lit_attrs[2].binding = 0;
        lit_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[2].offset = 6 * sizeof(float);
        lit_attrs[3].location = 3;
        lit_attrs[3].binding = 0;
        lit_attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[3].offset = 8 * sizeof(float);

        pipeline_lit_sprite_scene_ = create_pipeline(vert_lit_module_, frag_lit_sprite_module_, lit_pipeline_layout_,
                                                     scene_rp, sizeof(LitVertex2D), lit_attrs, 4,
                                                     false, false, BlendMode::Alpha);
    }

    // 全屏后处理顶点属性
    VkVertexInputAttributeDescription fs_attrs[2]{};
    fs_attrs[0].location = 0;
    fs_attrs[0].binding = 0;
    fs_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[0].offset = 0;
    fs_attrs[1].location = 1;
    fs_attrs[1].binding = 0;
    fs_attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[1].offset = 2 * sizeof(float);

    pipeline_bloom_threshold_ = create_pipeline(vert_bloom_module_, frag_bloom_threshold_module_,
                                                bloom_pipeline_layout_, bloom_rp,
                                                sizeof(FSVertex), fs_attrs, 2, false, false, BlendMode::Opaque);
    pipeline_bloom_blur_ = create_pipeline(vert_bloom_module_, frag_bloom_blur_module_,
                                           bloom_pipeline_layout_, bloom_rp,
                                           sizeof(FSVertex), fs_attrs, 2, false, false, BlendMode::Opaque);
    pipeline_bloom_compose_ = create_pipeline(vert_bloom_module_, frag_bloom_compose_module_,
                                              bloom_compose_pipeline_layout_, swapchain_rp,
                                              sizeof(FSVertex), fs_attrs, 2, false, false, BlendMode::Opaque);

    if (pipeline_rect_scene_ == VK_NULL_HANDLE || pipeline_text_scene_ == VK_NULL_HANDLE ||
        pipeline_sprite_scene_ == VK_NULL_HANDLE ||
        (frag_lit_sprite_module_ != VK_NULL_HANDLE && pipeline_lit_sprite_scene_ == VK_NULL_HANDLE) ||
        pipeline_bloom_threshold_ == VK_NULL_HANDLE || pipeline_bloom_blur_ == VK_NULL_HANDLE ||
        pipeline_bloom_compose_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create bloom pipelines");
        destroy_bloom_targets();
        return false;
    }

    bloom_initialized_ = true;
    return true;
}

void VulkanRenderer2D::destroy_bloom_targets() {
    destroy_bloom_pipelines();
    if (context_alive()) {
        if (scene_fb_.is_valid()) ctx_->destroy_framebuffer(scene_fb_);
        if (bloom_fb_a_.is_valid()) ctx_->destroy_framebuffer(bloom_fb_a_);
        if (bloom_fb_b_.is_valid()) ctx_->destroy_framebuffer(bloom_fb_b_);
        if (scene_texture_.is_valid()) ctx_->destroy_texture(scene_texture_);
        if (bloom_texture_a_.is_valid()) ctx_->destroy_texture(bloom_texture_a_);
        if (bloom_texture_b_.is_valid()) ctx_->destroy_texture(bloom_texture_b_);
    }
    scene_fb_ = RHIFramebufferHandle{};
    bloom_fb_a_ = RHIFramebufferHandle{};
    bloom_fb_b_ = RHIFramebufferHandle{};
    scene_texture_ = RHITextureHandle{};
    bloom_texture_a_ = RHITextureHandle{};
    bloom_texture_b_ = RHITextureHandle{};
    bloom_initialized_ = false;
}

void VulkanRenderer2D::destroy_bloom_pipelines() {
    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (dev == VK_NULL_HANDLE) return;

    auto destroy_if = [&](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    destroy_if(pipeline_rect_scene_);
    destroy_if(pipeline_text_scene_);
    destroy_if(pipeline_sprite_scene_);
    destroy_if(pipeline_sprite_scene_additive_);
    destroy_if(pipeline_lit_sprite_scene_);
    destroy_if(pipeline_bloom_threshold_);
    destroy_if(pipeline_bloom_blur_);
    destroy_if(pipeline_bloom_compose_);
}

void VulkanRenderer2D::write_lit_descriptor_set(VkDescriptorSet set, int frame_index,
                                                VulkanTexture* albedo, VulkanTexture* normal,
                                                bool use_shadow) {
    if (set == VK_NULL_HANDLE || !fallback_albedo_) return;
    if (frame_index < 0 || frame_index >= static_cast<int>(light_ubo_.size())) return;

    VkDevice dev = vk_device_->device();

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = light_ubo_[frame_index].buffer();
    buffer_info.offset = 0;
    buffer_info.range = sizeof(LightUBO);

    VulkanTexture* albedo_tex = albedo && albedo->image_view() ? albedo : fallback_albedo_;
    VulkanTexture* normal_tex = normal && normal->image_view() ? normal : fallback_normal_;

    VkDescriptorImageInfo infos[4]{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView = albedo_tex->image_view();
    infos[0].sampler = albedo_tex->sampler();

    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView = normal_tex->image_view();
    infos[1].sampler = normal_tex->sampler();

    infos[2].imageLayout = use_shadow && shadow_map_ ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[2].imageView = use_shadow && shadow_map_ ? shadow_map_->image_view() : fallback_albedo_->image_view();
    infos[2].sampler = use_shadow && shadow_map_ ? shadow_map_->sampler() : fallback_albedo_->sampler();

    VkWriteDescriptorSet writes[4]{};
    // binding 0: albedo
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &infos[0];
    // binding 1: normal
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &infos[1];
    // binding 2: lights UBO
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].dstArrayElement = 0;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &buffer_info;
    // binding 3: shadow map
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].dstArrayElement = 0;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &infos[2];

    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
}

void VulkanRenderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    sprite_vertices_.clear();
    sprite_texture_ = RHITextureHandle{};
    sprite_segments_.clear();
    rect_stream_base_ = 0;
    sprite_stream_base_ = 0;
    text_stream_base_ = 0;
    lit_batches_.clear();
    shadow_caster_vertices_.clear();
    reset_lights();
    // 描述符池 reset 与退役缓冲销毁必须在渲染线程执行：本函数跑在主线程，
    // 主线程可能比渲染线程领先最多 3 帧，直接在此 reset 会与渲染线程正在
    // 进行的 allocate/write 竞争（host 侧数据竞争，描述符损坏导致 GPU
    // device lost）。推成渲染命令后，它在 backend begin_frame（fence 等待）
    // 之后执行，时序与安全性都有保证。
    ctx_->push_command([this](IRenderBackend*) {
        int frame_index = vk_swapchain_ ? vk_swapchain_->current_frame_index() : 0;
        for (auto& dirty : descriptor_pool_dirty_) dirty = 1;
        if (frame_index >= 0 && frame_index < static_cast<int>(retired_buffers_.size())) {
            retired_buffers_[frame_index].clear();
        }
    });
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    GLOG_DEBUG("VulkanRenderer2D::begin_frame screen={}x{} cam_center=({}, {}) zoom={}",
              screen_width_, screen_height_, camera_center_.x, camera_center_.y, camera_zoom_);
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width, screen_height, 0.0f, -1.0f, 1.0f);

    math::Matrix4f view;
    if (camera_top_left_origin_) {
        view = math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    } else {
        math::Vector2f screen_center(screen_width * 0.5f, screen_height * 0.5f);
        view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
             * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    }
    view_proj_ = ortho_ * view;

    // bloom 目标的销毁/创建含直接 vkCreate*/vkDestroy*（pipeline、framebuffer、
    // VMA image）与资源池访问，渲染线程运行中会与其命令执行并发：
    // 池数据竞争、销毁渲染线程命令仍在引用的 pipeline、上传路径并发 queue submit。
    // 先暂停渲染线程（join + vkDeviceWaitIdle + drain）再重建，完成后恢复（对齐 GL 侧）。
    if (bloom_params_.enabled && !bloom_initialized_ && screen_width_ > 0.0f && screen_height_ > 0.0f) {
        if (ctx_->is_running()) {
            ctx_->pause_render_thread();
            create_bloom_targets();
            ctx_->resume_render_thread();
        } else {
            create_bloom_targets();
        }
    }
    if (bloom_params_.enabled && bloom_initialized_) {
        ITexture* scene_tex = ctx_->texture(scene_texture_);
        bool need_recreate = !scene_tex ||
                             static_cast<int>(scene_tex->width()) != static_cast<int>(screen_width_) ||
                             static_cast<int>(scene_tex->height()) != static_cast<int>(screen_height_);
        if (need_recreate) {
            if (ctx_->is_running()) {
                ctx_->pause_render_thread();
                destroy_bloom_targets();
                create_bloom_targets();
                ctx_->resume_render_thread();
            } else {
                destroy_bloom_targets();
                create_bloom_targets();
            }
        }
    }

    // 每帧默认回到 Alpha 混合，避免上一帧的 Additive 状态泄漏。
    set_blend_mode(BlendMode::Alpha);
}

void VulkanRenderer2D::set_camera(const math::Vector2f& center, float zoom, bool top_left_origin,
                                  float rotation) {
    // 切换摄像机前必须先提交当前已缓冲的顶点，确保它们使用绘制时的 view_proj 矩阵，
    // 否则 UI 层（top_left_origin）顶点会在后续 flush 时被普通摄像机矩阵变换到屏幕中心。
    flush_batches();
    flush_sprite_batch();

    camera_center_ = center;
    camera_zoom_ = zoom <= 0.0f ? 1.0f : zoom;
    camera_rotation_ = rotation;
    camera_top_left_origin_ = top_left_origin;
    if (screen_width_ > 0.0f && screen_height_ > 0.0f) {
        math::Matrix4f view;
        if (top_left_origin) {
            view = math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                 * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
                 * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        } else {
            math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
            view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                 * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                 * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
                 * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        }
        view_proj_ = ortho_ * view;
    }
}

float VulkanRenderer2D::overlay_scale() const {
    // 是否超采样是启动期的静态开关，与"当前是否在 overlay 内"无关。调用方要
    // 在 begin_overlay_pass 之前就把字号报上来——图集必须在那之前重烘完，
    // 在本帧已经绑定覆盖层目标之后重建纹理，会让目标失效、整屏画不出来。
    static const bool enabled = []() {
        const char* v = std::getenv("GRYCE_UI_SSAA");
        return !(v != nullptr && v[0] == '0');
    }();
    return (enabled && frag_ui_resolve_module_ != VK_NULL_HANDLE) ? 2.0f : 1.0f;
}

void VulkanRenderer2D::begin_overlay_pass(int screen_width, int screen_height) {
    if (!context_alive() || screen_width <= 0 || screen_height <= 0) return;

    // 超采样目标必须在 flush 之前准备好：尺寸变化时会创建/销毁纹理，得先于
    // 本帧的命令录制完成。
    float scale = 1.0f;
    if (overlay_scale() > 1.0f && ensure_ui_ssaa_target(screen_width, screen_height)) {
        scale = 2.0f;
        ctx_->set_framebuffer(ui_ssaa_fb_);
    } else {
        ctx_->set_framebuffer(RHIFramebufferHandle{});
    }
    overlay_scale_ = scale;

    flush_batches();
    flush_sprite_batch();

    overlay_saved_center_ = camera_center_;
    overlay_saved_zoom_ = camera_zoom_;
    overlay_saved_rotation_ = camera_rotation_;
    overlay_saved_top_left_ = camera_top_left_origin_;
    overlay_saved_width_ = screen_width_;
    overlay_saved_height_ = screen_height_;
    overlay_saved_active_ = true;
    overlay_pass_ = true;
    use_bloom_this_frame_ = false;

    screen_width_ = static_cast<float>(screen_width) * overlay_scale_;
    screen_height_ = static_cast<float>(screen_height) * overlay_scale_;
    camera_center_ = math::Vector2f::zero();
    camera_zoom_ = 1.0f;
    camera_rotation_ = 0.0f;
    camera_top_left_origin_ = true;
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width_, screen_height_,
                                   0.0f, -1.0f, 1.0f);
    view_proj_ = ortho_;

    set_blend_mode(BlendMode::Alpha);
    ctx_->set_viewport(0, 0, static_cast<int>(screen_width_), static_cast<int>(screen_height_));
    ctx_->set_depth_test(false);
    ctx_->set_blend(true);
    ctx_->set_cull_face(CullMode::None);
    // 覆盖层总是以"满屏可写"开局：上一帧末尾留下的裁剪框（滚动区、下拉列表）
    // 不能漏进这一帧，否则第二次进入时整片界面会被裁掉。
    apply_scissor_rect(0, 0, screen_width, screen_height);
}

void VulkanRenderer2D::end_overlay_pass() {
    if (!overlay_saved_active_) return;
    flush_batches();
    flush_sprite_batch();
    overlay_pass_ = false;
    overlay_saved_active_ = false;

    // 超采样解析必须在恢复屏幕尺寸之前判定，之后 overlay_scale_ 就归 1 了。
    const bool resolve_ssaa = overlay_scale_ > 1.5f && ui_ssaa_ready_ &&
                              ui_ssaa_texture_.is_valid() &&
                              pipeline_ui_resolve_ != VK_NULL_HANDLE;
    overlay_scale_ = 1.0f;

    screen_width_ = overlay_saved_width_;
    screen_height_ = overlay_saved_height_;
    camera_center_ = overlay_saved_center_;
    camera_zoom_ = overlay_saved_zoom_;
    camera_rotation_ = overlay_saved_rotation_;
    camera_top_left_origin_ = overlay_saved_top_left_;
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width_, screen_height_,
                                   0.0f, -1.0f, 1.0f);
    math::Matrix4f view;
    if (camera_top_left_origin_) {
        view = math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_,
                                      math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    } else {
        const math::Vector2f screen_center(screen_width_ * 0.5f,
                                           screen_height_ * 0.5f);
        view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
             * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_,
                                      math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    }
    view_proj_ = ortho_ * view;

    // 降采样：把 2x 覆盖层按线性过滤画回 back buffer。输出像素与输入像素
    // 精确 2:1 对齐，双线性在四 texel 交界处权重各 0.25，等价于 2x2 盒式平均。
    if (resolve_ssaa) {
        draw_fullscreen_pass(RHIFramebufferHandle{}, pipeline_ui_resolve_,
                             bloom_pipeline_layout_, false,
                             {{ui_ssaa_texture_, 0}}, nullptr, 0);
    }

    // 交还控制权前恢复成整个目标：这一帧后面还有绘制时（如 FPS 回退文本），
    // 不该继承最后一处 UI 的裁剪矩形。
    const int full_w = static_cast<int>(screen_width_);
    const int full_h = static_cast<int>(screen_height_);
    ctx_->push_command([this, full_w, full_h](IRenderBackend*) {
        vk_backend_->set_scissor(0, 0, full_w, full_h);
    });
}

void VulkanRenderer2D::apply_scissor_rect(int x, int y, int w, int h) {
    if (!context_alive() || !vk_backend_ || !ctx_) return;
    const float scale = overlay_scale_;
    int sx = static_cast<int>(std::floor(static_cast<float>(x) * scale));
    int sy = static_cast<int>(std::floor(static_cast<float>(y) * scale));
    int sw = static_cast<int>(std::ceil(static_cast<float>(w) * scale));
    int sh = static_cast<int>(std::ceil(static_cast<float>(h) * scale));
    // 负宽高在 Vulkan 里是非法矩形；越界部分按目标尺寸夹掉，夹空就是一个
    // 全零的裁剪框（合法的"什么都不画"）。
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    if (sx < 0) { sw = std::max(0, sw + sx); sx = 0; }
    if (sy < 0) { sh = std::max(0, sh + sy); sy = 0; }
    const int limit_w = static_cast<int>(screen_width_);
    const int limit_h = static_cast<int>(screen_height_);
    sw = std::min(sw, std::max(0, limit_w - sx));
    sh = std::min(sh, std::max(0, limit_h - sy));
    ctx_->push_command([this, sx, sy, sw, sh](IRenderBackend*) {
        vk_backend_->set_scissor(sx, sy, sw, sh);
    });
    scissor_set_ = true;
    scissor_rect_ = {{sx, sy, sw, sh}};
}

void VulkanRenderer2D::set_scissor(int x, int y, int w, int h) {
    if (!context_alive()) return;
    // 先提交已积累的顶点：顶点流是"一次上传 + 按 firstVertex 分段绘制"，
    // 中途改裁剪区却不提交，早先排队的图元会跟着吃到新矩形。
    flush_batches();
    apply_scissor_rect(x, y, w, h);
}

void VulkanRenderer2D::reset_scissor() {
    if (!context_alive()) return;
    flush_batches();
    const int w = static_cast<int>(screen_width_);
    const int h = static_cast<int>(screen_height_);
    ctx_->push_command([this, w, h](IRenderBackend*) {
        vk_backend_->set_scissor(0, 0, w, h);
    });
    scissor_set_ = false;
    scissor_rect_ = {{0, 0, w, h}};
}

math::Vector2f VulkanRenderer2D::world_to_screen(const math::Vector2f& world) const {
    const float c = std::cos(-camera_rotation_);
    const float s = std::sin(-camera_rotation_);
    const math::Vector2f d = world - camera_center_;
    const math::Vector2f rotated(d.x * c - d.y * s, d.x * s + d.y * c);
    if (camera_top_left_origin_) {
        return rotated * camera_zoom_;
    }
    math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
    return screen_center + rotated * camera_zoom_;
}

void VulkanRenderer2D::end_frame() {
    use_bloom_this_frame_ = bloom_params_.enabled && bloom_initialized_;
    bool did_shadow = !shadow_caster_vertices_.empty() && pipeline_shadow_ != VK_NULL_HANDLE && shadow_fb_.is_valid();

    // 1. 阴影 pass（在场景渲染前完成，lit sprite 采样）
    if (did_shadow) {
        render_shadow_pass();
    }

    // 2. 绑定到场景 FBO（Bloom）或回到 swapchain（刚完成阴影 pass）
    if (use_bloom_this_frame_) {
        ctx_->push_command([this](IRenderBackend*) {
            vk_backend_->bind_framebuffer(scene_fb_);
        });
    } else if (did_shadow) {
        ctx_->push_command([this](IRenderBackend*) {
            vk_backend_->unbind_framebuffer();
        });
    }

    // 3. 渲染不受光照的 2D 几何体
    flush_batches();
    flush_sprite_batch();

    // 4. 前向光照
    if (!lit_batches_.empty() && lit_pipeline_layout_ != VK_NULL_HANDLE) {
        render_lit_sprites_forward(use_bloom_this_frame_);
    }

    // 5. Bloom 后处理
    if (use_bloom_this_frame_) {
        render_bloom_pass();
    }
}

void VulkanRenderer2D::set_blend_mode(BlendMode mode) {
    if (mode == blend_mode_) return;

    // 切换混合模式前必须先提交当前精灵批次，因为 Vulkan 的混合状态由 Pipeline 决定。
    flush_sprite_batch();
    blend_mode_ = mode;
}

void VulkanRenderer2D::render_shadow_pass() {
    if (!ctx_ || shadow_caster_vertices_.empty() || !shadow_fb_.is_valid() ||
        pipeline_shadow_ == VK_NULL_HANDLE) {
        return;
    }

    // 找到第一个投射阴影的光源（Directional 优先，否则 Spot）
    const Light2D* shadow_light = nullptr;
    int shadow_light_index = -1;
    for (int i = 0; i < static_cast<int>(lights_.size()); ++i) {
        if (lights_[i].type == LightType2D::Directional || lights_[i].type == LightType2D::Spot) {
            shadow_light = &lights_[i];
            shadow_light_index = i;
            break;
        }
    }
    if (!shadow_light) return;

    math::Vector2f dir = shadow_light->direction.normalized();
    if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);

    math::Vector2f center = camera_center_;
    float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
    math::Vector2f eye = center - dir * view_size;
    math::Matrix4f light_view = math::Matrix4f::look_at(
        math::Vector3f(eye.x, eye.y, 0.0f),
        math::Vector3f(center.x, center.y, 0.0f),
        math::Vector3f(0.0f, 0.0f, 1.0f));
    math::Matrix4f light_proj = math::Matrix4f::ortho(
        -view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
    math::Matrix4f light_space = light_proj * light_view;

    auto verts_shared = std::make_shared<std::vector<ShadowCasterVertex>>(std::move(shadow_caster_vertices_));
    shadow_caster_vertices_.clear();

    ctx_->push_command([this, verts_shared, light_space](IRenderBackend*) {
        vk_backend_->bind_framebuffer(shadow_fb_);

        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_shadow_caster_.size())) {
            return;
        }

        VulkanBuffer* vertex_buffer = &vertex_buffer_shadow_caster_[frame_index];
        VkDeviceSize* capacity = &vertex_buffer_shadow_caster_capacity_[frame_index];
        VkDeviceSize required = verts_shared->size() * sizeof(ShadowCasterVertex);
        if (!grow_vertex_buffer(*vertex_buffer, *capacity, required, frame_index,
                                "shadow caster vertex buffer")) {
            return;
        }
        vertex_buffer->upload(verts_shared->data(), required);

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(k_shadow_map_size);
        viewport.height = static_cast<float>(k_shadow_map_size);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(k_shadow_map_size), static_cast<uint32_t>(k_shadow_map_size)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline_shadow_);

        if (vk_backend_->supports_dynamic_state()) {
            vk_backend_->set_cull_mode_cached(cmd, VK_CULL_MODE_NONE);
            vk_backend_->set_front_face_cached(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vk_backend_->set_depth_test_cached(cmd, VK_TRUE);
            vk_backend_->set_depth_write_cached(cmd, VK_TRUE);
        }

        vkCmdPushConstants(cmd, shadow_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(math::Matrix4f), &light_space);

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, static_cast<uint32_t>(verts_shared->size()), 1, 0, 0);
    });
}

void VulkanRenderer2D::render_lit_sprites_forward(bool offscreen) {
    if (lit_batches_.empty() || !lit_pipeline_layout_) return;

    VkPipeline lit_pipeline = offscreen ? pipeline_lit_sprite_scene_ : pipeline_lit_sprite_;
    if (lit_pipeline == VK_NULL_HANDLE) return;

    // 构建光源 UBO
    LightUBO ubo{};
    ubo.ambient = math::Vector3f(ambient_light_.r, ambient_light_.g, ambient_light_.b);
    int light_count = static_cast<int>(std::min<size_t>(lights_.size(), k_max_lights));
    ubo.light_count = light_count;
    for (int i = 0; i < light_count; ++i) {
        const Light2D& L = lights_[i];
        LightData& d = ubo.lights[i];
        d.type = static_cast<int>(L.type);
        d.pos = L.position;
        d.dir = L.direction;
        d.color[0] = L.color.r;
        d.color[1] = L.color.g;
        d.color[2] = L.color.b;
        d.intensity = L.intensity;
        d.radius = L.radius;
        d.range = L.range;
        d.spot_angle = L.spot_angle;
        d.spot_softness = L.spot_softness;
    }

    int shadow_light_index = -1;
    for (int i = 0; i < light_count; ++i) {
        if (lights_[i].type == LightType2D::Directional || lights_[i].type == LightType2D::Spot) {
            shadow_light_index = i;
            break;
        }
    }
    bool use_shadow = (shadow_light_index >= 0 && shadow_fb_.is_valid() && shadow_map_);
    ubo.use_shadow_map = use_shadow ? 1 : 0;
    ubo.shadow_light_index = shadow_light_index;
    if (use_shadow) {
        const Light2D& shadow_light = lights_[shadow_light_index];
        math::Vector2f dir = shadow_light.direction.normalized();
        if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);
        math::Vector2f center = camera_center_;
        float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
        math::Vector2f eye = center - dir * view_size;
        math::Matrix4f light_view = math::Matrix4f::look_at(
            math::Vector3f(eye.x, eye.y, 0.0f),
            math::Vector3f(center.x, center.y, 0.0f),
            math::Vector3f(0.0f, 0.0f, 1.0f));
        math::Matrix4f light_proj = math::Matrix4f::ortho(
            -view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
        ubo.light_space_matrix = light_proj * light_view;
    }

    // 合并所有批次到一个顶点数组。旧实现每个批次都往同一个顶点缓冲的
    // offset 0 上传：memcpy 发生在命令录制期间，后一批会覆盖前一批的顶点，
    // 导致所有 draw 实际都读到最后一批的几何（只有最后一个批次可见）。
    struct BatchSpan {
        RHITextureHandle albedo;
        RHITextureHandle normal;
        uint32_t first_vertex;
        uint32_t vertex_count;
    };
    std::vector<BatchSpan> spans;
    std::vector<LitVertex2D> all_verts;
    spans.reserve(lit_batches_.size());
    for (auto& kv : lit_batches_) {
        if (kv.second.verts.empty()) continue;
        spans.push_back({kv.second.albedo, kv.second.normal,
                         static_cast<uint32_t>(all_verts.size()),
                         static_cast<uint32_t>(kv.second.verts.size())});
        all_verts.insert(all_verts.end(),
                         std::make_move_iterator(kv.second.verts.begin()),
                         std::make_move_iterator(kv.second.verts.end()));
    }
    lit_batches_.clear();
    if (all_verts.empty()) return;

    // 上传 UBO
    auto ubo_shared = std::make_shared<LightUBO>(std::move(ubo));
    ctx_->push_command([this, ubo_shared](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(light_ubo_.size())) return;
        light_ubo_[frame_index].upload(ubo_shared.get(), sizeof(LightUBO));
    });

    // 单次上传顶点缓冲，逐批次用 firstVertex 偏移绘制
    auto verts_shared = std::make_shared<std::vector<LitVertex2D>>(std::move(all_verts));
    auto spans_shared = std::make_shared<std::vector<BatchSpan>>(std::move(spans));
    ctx_->push_command([this, verts_shared, spans_shared, lit_pipeline, use_shadow](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_lit_sprite_.size())) return;

        VulkanBuffer* vertex_buffer = &vertex_buffer_lit_sprite_[frame_index];
        VkDeviceSize* capacity = &vertex_buffer_lit_sprite_capacity_[frame_index];
        VkDeviceSize required = verts_shared->size() * sizeof(LitVertex2D);
        if (!grow_vertex_buffer(*vertex_buffer, *capacity, required, frame_index,
                                "lit sprite vertex buffer")) {
            return;
        }
        vertex_buffer->upload(verts_shared->data(), required);

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = screen_height_;
        viewport.width = screen_width_;
        viewport.height = -screen_height_;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(screen_width_), static_cast<uint32_t>(screen_height_)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, lit_pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        vkCmdPushConstants(cmd, lit_pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(math::Matrix4f), &view_proj_);

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);

        for (const auto& span : *spans_shared) {
            VkDescriptorSet set = allocate_descriptor_set(lit_descriptor_layout_);
            if (set == VK_NULL_HANDLE) {
                GLOG_ERROR("VulkanRenderer2D::render_lit_sprites_forward failed to allocate descriptor set");
                continue;
            }
            // 句柄在执行时经 generation 校验解析；纹理已销毁则回退默认纹理
            auto* vk_albedo = span.albedo.is_valid()
                ? dynamic_cast<VulkanTexture*>(ctx_->texture(span.albedo)) : nullptr;
            auto* vk_normal = span.normal.is_valid()
                ? dynamic_cast<VulkanTexture*>(ctx_->texture(span.normal)) : nullptr;
            write_lit_descriptor_set(set, frame_index, vk_albedo, vk_normal, use_shadow);
            vk_backend_->bind_descriptor_set(cmd, lit_pipeline_layout_, set);
            vkCmdDraw(cmd, span.vertex_count, 1, span.first_vertex, 0);
        }
    });
}

void VulkanRenderer2D::render_bloom_pass() {
    if (!bloom_initialized_) return;

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);
    float inv_w = 1.0f / static_cast<float>(w);
    float inv_h = 1.0f / static_cast<float>(h);

    // 1. Threshold: scene -> bloom_a
    draw_fullscreen_pass(bloom_fb_a_, pipeline_bloom_threshold_, bloom_pipeline_layout_,
                         false, {{scene_texture_, 0}},
                         &bloom_params_.threshold, sizeof(bloom_params_.threshold));

    // 2. Blur passes: bloom_a <-> bloom_b
    RHITextureHandle src = bloom_texture_a_;
    RHIFramebufferHandle dst = bloom_fb_b_;
    for (int i = 0; i < bloom_params_.blur_passes * 2; ++i) {
        bool horizontal = (i % 2) == 0;
        math::Vector2f dir = horizontal ? math::Vector2f(1.0f, 0.0f) : math::Vector2f(0.0f, 1.0f);
        math::Vector4f push(dir.x, dir.y, inv_w, inv_h);
        draw_fullscreen_pass(dst, pipeline_bloom_blur_, bloom_pipeline_layout_,
                             false, {{src, 0}},
                             &push, sizeof(push));
        std::swap(src, bloom_texture_a_);
        std::swap(dst, bloom_fb_a_);
    }
    // 最终 bloom 结果在 src 中

    // 3. Compose: scene + bloom -> backbuffer
    draw_fullscreen_pass(RHIFramebufferHandle{}, pipeline_bloom_compose_, bloom_compose_pipeline_layout_,
                         true, {{scene_texture_, 0}, {src, 1}},
                         &bloom_params_.intensity, sizeof(bloom_params_.intensity));
}

void VulkanRenderer2D::draw_fullscreen_pass(RHIFramebufferHandle fb,
                                             VkPipeline pipeline, VkPipelineLayout layout,
                                             bool use_lit_descriptor_set,
                                             const std::vector<std::pair<RHITextureHandle, uint32_t>>& bindings,
                                             const void* push_constants, size_t push_size) {
    if (pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) return;
    if (push_size > 16) return;

    float screen_w = screen_width_;
    float screen_h = screen_height_;

    std::array<uint8_t, 16> push_data{};
    if (push_size > 0) {
        std::memcpy(push_data.data(), push_constants, push_size);
    }

    ctx_->push_command([=, this](IRenderBackend*) mutable {
        if (fb.is_valid()) {
            vk_backend_->bind_framebuffer(fb);
        } else {
            vk_backend_->unbind_framebuffer();
        }

        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(fs_vertex_buffer_.size())) return;

        VkDescriptorSetLayout set_layout = use_lit_descriptor_set ? lit_descriptor_layout_ : descriptor_layout_;
        VkDescriptorSet set = allocate_descriptor_set(set_layout);
        if (set == VK_NULL_HANDLE) {
            GLOG_ERROR("VulkanRenderer2D::draw_fullscreen_pass failed to allocate descriptor set");
            return;
        }

        VulkanBuffer* vertex_buffer = &fs_vertex_buffer_[frame_index];

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = screen_h;
        viewport.width = screen_w;
        viewport.height = -screen_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        // 更新 descriptor image bindings（最多 2 个 binding：scene + bloom）
        std::array<VkDescriptorImageInfo, 2> image_infos{};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (size_t i = 0; i < bindings.size(); ++i) {
            ITexture* tex = ctx_->texture(bindings[i].first);
            auto* vk_tex = dynamic_cast<VulkanTexture*>(tex);
            if (!vk_tex || !vk_tex->image_view()) vk_tex = fallback_albedo_;
            image_infos[i].imageLayout = (bindings[i].second == 3)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos[i].imageView = vk_tex->image_view();
            image_infos[i].sampler = vk_tex->sampler();

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = bindings[i].second;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &image_infos[i];
        }
        if (!bindings.empty()) {
            vkUpdateDescriptorSets(vk_device_->device(), static_cast<uint32_t>(bindings.size()), writes.data(), 0, nullptr);
        }

        vk_backend_->bind_descriptor_set(cmd, layout, set);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           static_cast<uint32_t>(push_size), push_data.data());

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
}

void VulkanRenderer2D::push_vertex(float x, float y, const Color& color, float u, float v) {
    // 覆盖层超采样时几何要按倍率铺到更大的离屏目标上。文字顶点不在这里乘：
    // draw_text 内部已经按 overlay_scale_ 换了挡位和笔位，再乘一次会双倍放大。
    if (overlay_scale_ != 1.0f) {
        x *= overlay_scale_;
        y *= overlay_scale_;
    }
    vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_text_vertex(float x, float y, const Color& color, float u, float v) {
    text_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_sprite_vertex(float x, float y, const Color& color, float u, float v) {
    if (overlay_scale_ != 1.0f) {
        x *= overlay_scale_;
        y *= overlay_scale_;
    }
    sprite_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_lit_vertex(RHITextureHandle albedo, RHITextureHandle normal,
                                        float x, float y, const Color& color,
                                        float u, float v, float nu, float nv) {
    LitBatch* batch = find_lit_batch(albedo, normal);
    if (batch) {
        batch->verts.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
    }
}

void VulkanRenderer2D::push_shadow_caster_vertex(float x, float y) {
    shadow_caster_vertices_.push_back({x, y});
}

VulkanRenderer2D::LitBatch* VulkanRenderer2D::find_lit_batch(RHITextureHandle albedo, RHITextureHandle normal) {
    LitBatchKey key{albedo, normal};
    auto it = lit_batches_.find(key);
    if (it == lit_batches_.end()) {
        LitBatch batch;
        batch.albedo = albedo;
        batch.normal = normal;
        it = lit_batches_.emplace(key, std::move(batch)).first;
    }
    return &it->second;
}

VkPipeline VulkanRenderer2D::current_sprite_pipeline() const {
    if (use_bloom_this_frame_ && !overlay_pass_) {
        return (blend_mode_ == BlendMode::Additive && pipeline_sprite_scene_additive_ != VK_NULL_HANDLE)
                   ? pipeline_sprite_scene_additive_
                   : pipeline_sprite_scene_;
    }
    return (blend_mode_ == BlendMode::Additive && pipeline_sprite_additive_ != VK_NULL_HANDLE)
               ? pipeline_sprite_additive_
               : pipeline_sprite_;
}

void VulkanRenderer2D::finalize_sprite_segment() {
    if (sprite_vertices_.empty() || !sprite_texture_.is_valid()) {
        sprite_vertices_.clear();
        sprite_texture_ = RHITextureHandle{};
        return;
    }
    SpriteSegment segment;
    segment.texture = sprite_texture_;
    // 混合/管线在封口时定下来：之后调用方再改混合模式也不会影响已绘制的这一段。
    segment.pipeline = current_sprite_pipeline();
    segment.vertices = std::move(sprite_vertices_);
    segment.rect_vertex_split = vertices_.size();
    sprite_segments_.push_back(std::move(segment));
    sprite_vertices_.clear();
    sprite_texture_ = RHITextureHandle{};
}

void VulkanRenderer2D::flush_batches() {
    finalize_sprite_segment();

    const bool bloom = use_bloom_this_frame_ && !overlay_pass_;
    VkPipeline rect_pipeline = bloom ? pipeline_rect_scene_ : pipeline_rect_;
    VkPipeline text_pipeline = bloom ? pipeline_text_scene_ : pipeline_text_;

    // 每个帧槽只有一个矩形顶点缓冲、一个贴图顶点缓冲，所以两种流都只能上传一次：
    // 分段上传会让后一段把前一段的数据冲掉（表现为整片发白、只剩最后一张图）。
    // 这里各自拼成一条流，用 firstVertex 让每段取自己那段顶点。
    std::vector<Vertex2D> rect_stream = std::move(vertices_);
    vertices_.clear();
    const std::size_t rect_total = rect_stream.size();

    struct SpriteDraw {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        ITexture* texture = nullptr;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::size_t rect_split = 0;
    };
    std::vector<Vertex2D> sprite_stream;
    std::vector<SpriteDraw> sprite_draws;
    sprite_draws.reserve(sprite_segments_.size());
    for (auto& segment : sprite_segments_) {
        ITexture* tex = ctx_ ? ctx_->texture(segment.texture) : nullptr;
        if (!tex) {
            GLOG_WARN("VulkanRenderer2D::flush_batches: sprite texture expired, dropping {} verts",
                      segment.vertices.size());
            continue;
        }
        if (segment.vertices.empty()) continue;
        SpriteDraw draw;
        draw.first = static_cast<std::uint32_t>(sprite_stream.size());
        draw.count = static_cast<std::uint32_t>(segment.vertices.size());
        draw.texture = tex;
        draw.pipeline = segment.pipeline;
        draw.rect_split = std::min(segment.rect_vertex_split, rect_total);
        sprite_stream.insert(sprite_stream.end(), segment.vertices.begin(), segment.vertices.end());
        sprite_draws.push_back(draw);
    }
    sprite_segments_.clear();
    // 必须先记下条数：第一段 flush 会把 sprite_stream 整体 move 走，之后再取
    // size() 拿到的是 0，跨批次基址就不再前进 —— 后一次上传会从头覆盖前一批
    // 的顶点，而先前录制的 vkCmdDraw 仍指向那些旧偏移（画出来是别的图元）。
    const std::size_t sprite_total = sprite_stream.size();

    bool rect_uploaded = false;
    bool sprite_uploaded = false;
    const std::uint32_t rect_base = rect_stream_base_;
    auto emit_rects = [&](std::size_t begin, std::size_t end) {
        if (end <= begin) return;
        const auto first = static_cast<std::uint32_t>(begin) + rect_base;
        const auto count = static_cast<std::uint32_t>(end - begin);
        if (!rect_uploaded) {
            rect_uploaded = true;
            flush_batch(std::move(rect_stream), false, nullptr, rect_pipeline,
                        pipeline_layout_, first, count, rect_base);
        } else {
            flush_batch({}, false, nullptr, rect_pipeline, pipeline_layout_, first, count);
        }
    };

    std::size_t emitted = 0;
    for (const SpriteDraw& draw : sprite_draws) {
        const std::size_t split = std::max(emitted, draw.rect_split);
        emit_rects(emitted, split);
        emitted = split;
        const std::uint32_t sprite_first = draw.first + sprite_stream_base_;
        if (!sprite_uploaded) {
            sprite_uploaded = true;
            flush_batch(std::move(sprite_stream), false, draw.texture, draw.pipeline,
                        pipeline_layout_, sprite_first, draw.count, sprite_stream_base_);
        } else {
            flush_batch({}, false, draw.texture, draw.pipeline, pipeline_layout_,
                        sprite_first, draw.count);
        }
    }
    emit_rects(emitted, rect_total);
    rect_stream_base_ += static_cast<std::uint32_t>(rect_total);
    sprite_stream_base_ += static_cast<std::uint32_t>(sprite_total);

    // 字体图集纹理保留裸指针：FontAtlas 由本对象拥有，生命周期一致，
    // 且 shutdown 时渲染线程已同步，不存在悬垂风险，无需 handle 化。
    if (!text_vertices_.empty()) {
        const auto text_count = static_cast<std::uint32_t>(text_vertices_.size());
        flush_batch(std::move(text_vertices_), true, font_atlas_.texture(), text_pipeline,
                    pipeline_layout_, text_stream_base_, text_count, text_stream_base_);
        text_stream_base_ += text_count;
        text_vertices_.clear();
    }
}

void VulkanRenderer2D::flush_sprite_batch() {
    // 只封口，不立即提交：提交时机统一由 flush_batches 控制，否则贴图会被后提交的
    // 矩形/文字盖住。段内已固化纹理与管线，晚提交不会混淆状态。
    finalize_sprite_segment();
}

void VulkanRenderer2D::flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                                    VkPipeline pipeline, VkPipelineLayout layout,
                                    std::uint32_t first_vertex, std::uint32_t draw_count,
                                    std::uint32_t upload_first_vertex) {
    const bool is_sprite = (!is_text && texture != nullptr);
    const bool upload = !verts.empty();
    const std::uint32_t count = draw_count != 0 ? draw_count
                                                : static_cast<std::uint32_t>(verts.size());

    if (count == 0 || !context_alive() || !vk_backend_ || pipeline == VK_NULL_HANDLE ||
        layout == VK_NULL_HANDLE) {
        return;
    }

    math::Matrix4f view_proj = view_proj_;
    ITexture* tex = texture;
    float screen_w = screen_width_;
    float screen_h = screen_height_;
    // 这批顶点积累期间的裁剪矩形：提交时可能早就换过，必须按快照下发。
    const bool scissor_set = scissor_set_;
    const std::array<std::int32_t, 4> scissor_rect = scissor_rect_;

    auto verts_shared = std::make_shared<std::vector<Vertex2D>>(std::move(verts));

    ctx_->push_command([this, verts_shared, is_text, is_sprite, tex, pipeline, layout,
                        view_proj, screen_w, screen_h, upload, count, first_vertex,
                        upload_first_vertex, scissor_set, scissor_rect](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_rect_.size())) {
            return;
        }

        VulkanBuffer* vertex_buffer = nullptr;
        VkDeviceSize* capacity = nullptr;
        if (is_text) {
            vertex_buffer = &vertex_buffer_text_[frame_index];
            capacity = &vertex_buffer_text_capacity_[frame_index];
        } else if (is_sprite) {
            vertex_buffer = &vertex_buffer_sprite_[frame_index];
            capacity = &vertex_buffer_sprite_capacity_[frame_index];
        } else {
            vertex_buffer = &vertex_buffer_rect_[frame_index];
            capacity = &vertex_buffer_rect_capacity_[frame_index];
        }

        if (upload) {
            VkDeviceSize required = (static_cast<VkDeviceSize>(upload_first_vertex) +
                                     verts_shared->size()) * sizeof(Vertex2D);
            if (!grow_vertex_buffer(*vertex_buffer, *capacity, required, frame_index,
                                    "vertex buffer")) {
                return;
            }
            vertex_buffer->upload(verts_shared->data(),
                                  verts_shared->size() * sizeof(Vertex2D),
                                  static_cast<VkDeviceSize>(upload_first_vertex) * sizeof(Vertex2D));
        } else if (vertex_buffer->buffer() == VK_NULL_HANDLE) {
            return;
        }

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = screen_h;
        viewport.width = screen_w;
        viewport.height = -screen_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        GLOG_DEBUG("VulkanRenderer2D::flush_batch viewport={} {} {} {} verts={}",
                  viewport.x, viewport.y, viewport.width, viewport.height, verts_shared->size());
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        if (scissor_set) {
            scissor.offset = {scissor_rect[0], scissor_rect[1]};
            scissor.extent = {static_cast<uint32_t>(scissor_rect[2]),
                              static_cast<uint32_t>(scissor_rect[3])};
        } else {
            scissor.offset = {0, 0};
            scissor.extent = {static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h)};
        }
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        struct alignas(16) PushConstants {
            math::Matrix4f view_proj;
        } pc;
        pc.view_proj = view_proj;
        GLOG_DEBUG("VulkanRenderer2D::flush_batch view_proj col0=({}, {}, {}, {}) col1=({}, {}, {}, {}) col3=({}, {}, {}, {})",
                  pc.view_proj(0,0), pc.view_proj(1,0), pc.view_proj(2,0), pc.view_proj(3,0),
                  pc.view_proj(0,1), pc.view_proj(1,1), pc.view_proj(2,1), pc.view_proj(3,1),
                  pc.view_proj(0,3), pc.view_proj(1,3), pc.view_proj(2,3), pc.view_proj(3,3));
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PushConstants), &pc);

        // Allocate a fresh descriptor set for this draw batch so we never update a
        // set that is already referenced by recorded commands.
        VkDescriptorSet set = allocate_descriptor_set(descriptor_layout_);
        if (set == VK_NULL_HANDLE) {
            GLOG_ERROR("VulkanRenderer2D::flush_batch failed to allocate descriptor set");
            return;
        }

        VulkanTexture* tex_to_bind = nullptr;
        if (is_text) {
            tex_to_bind = font_atlas_.texture() ? dynamic_cast<VulkanTexture*>(font_atlas_.texture()) : nullptr;
        } else if (is_sprite) {
            tex_to_bind = tex ? dynamic_cast<VulkanTexture*>(tex) : nullptr;
        } else {
            tex_to_bind = fallback_albedo_;
        }
        if (!tex_to_bind || !tex_to_bind->image_view() || !tex_to_bind->sampler()) {
            tex_to_bind = fallback_albedo_;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = tex_to_bind->image_view();
        image_info.sampler = tex_to_bind->sampler();

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(vk_device_->device(), 1, &write, 0, nullptr);

        vk_backend_->bind_descriptor_set(cmd, layout, set);

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, count, 1, first_vertex, 0);
    });
}

void VulkanRenderer2D::draw_rect(float x, float y, float w, float h, const Color& color) {
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);
    push_vertex(x0, y1, color, 0, 0);
}

void VulkanRenderer2D::draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) {
    if (points.size() < 3) return;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        push_vertex(points[0].x, points[0].y, color, 0, 0);
        push_vertex(points[i].x, points[i].y, color, 0, 0);
        push_vertex(points[i + 1].x, points[i + 1].y, color, 0, 0);
    }
}

void VulkanRenderer2D::draw_circle(float cx, float cy, float r, int segments, const Color& color) {
    if (segments < 3) segments = 3;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < segments; ++i) {
        float a0 = 2.0f * pi * static_cast<float>(i) / static_cast<float>(segments);
        float a1 = 2.0f * pi * static_cast<float>(i + 1) / static_cast<float>(segments);
        float x0 = cx + r * std::cos(a0);
        float y0 = cy + r * std::sin(a0);
        float x1 = cx + r * std::cos(a1);
        float y1 = cy + r * std::sin(a1);

        push_vertex(cx, cy, color, 0, 0);
        push_vertex(x0, y0, color, 0, 0);
        push_vertex(x1, y1, color, 0, 0);
    }
}

void VulkanRenderer2D::draw_round_rect(float x, float y, float w, float h,
                                       const float radii[4], const Color& color) {
    draw_round_rect_gradient(x, y, w, h, radii, color, color);
}

void VulkanRenderer2D::draw_round_rect_gradient(float x, float y, float w, float h,
                                                const float radii[4],
                                                const Color& top, const Color& bottom) {
    auto push = [this](float px, float py, const Color& c) {
        push_vertex(px, py, c, 0, 0);
    };
    render_detail::emit_round_fill(push, x, y, w, h, radii, top, bottom);
}

void VulkanRenderer2D::draw_round_rect_border(float x, float y, float w, float h,
                                              const float radii[4], float thickness,
                                              const Color& color) {
    auto push = [this](float px, float py, const Color& c) {
        push_vertex(px, py, c, 0, 0);
    };
    render_detail::emit_round_border(push, x, y, w, h, radii, thickness, color);
}

void VulkanRenderer2D::draw_round_shadow(float x, float y, float w, float h,
                                         const float radii[4], float spread,
                                         const Color& color) {
    auto push = [this](float px, float py, const Color& c) {
        push_vertex(px, py, c, 0, 0);
    };
    render_detail::emit_round_shadow(push, x, y, w, h, radii, spread, color);
}

void VulkanRenderer2D::draw_sprite(float x, float y, float w, float h,
                                    RHITextureHandle texture, const Color& tint) {
    draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

void VulkanRenderer2D::draw_sprite_region(float x, float y, float w, float h,
                                           float u0, float v0, float u1, float v1,
                                           RHITextureHandle texture, const Color& tint) {
    // 纹理统一传 RHITextureHandle：无效句柄回退纯色矩形（与旧裸指针空值行为一致）；
    // 有效句柄在 flush 时经 generation 校验解析，纹理已销毁则丢弃本批。
    if (!texture.is_valid()) {
        draw_rect(x, y, w, h, tint);
        return;
    }

    if (texture != sprite_texture_) {
        flush_sprite_batch();
        sprite_texture_ = texture;
    }

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_sprite_vertex(x0, y0, tint, u0, v0);
    push_sprite_vertex(x1, y0, tint, u1, v0);
    push_sprite_vertex(x1, y1, tint, u1, v1);

    push_sprite_vertex(x0, y0, tint, u0, v0);
    push_sprite_vertex(x1, y1, tint, u1, v1);
    push_sprite_vertex(x0, y1, tint, u0, v1);
}

void VulkanRenderer2D::draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                                            RHITextureHandle texture, const Color& tint) {
    if (!texture.is_valid()) {
        draw_rect(x, y, w, h, tint);
        return;
    }

    if (texture != sprite_texture_) {
        flush_sprite_batch();
        sprite_texture_ = texture;
    }

    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);

    const math::Vector2f local[4] = {
        math::Vector2f(-hw, -hh),
        math::Vector2f( hw, -hh),
        math::Vector2f( hw,  hh),
        math::Vector2f(-hw,  hh)
    };
    const math::Vector2f uv[4] = {
        math::Vector2f(0.0f, 0.0f),
        math::Vector2f(1.0f, 0.0f),
        math::Vector2f(1.0f, 1.0f),
        math::Vector2f(0.0f, 1.0f)
    };

    for (int i = 0; i < 4; ++i) {
        math::Vector2f p(
            cx + local[i].x * c - local[i].y * s,
            cy + local[i].x * s + local[i].y * c
        );
        push_sprite_vertex(p.x, p.y, tint, uv[i].x, uv[i].y);
    }

    // 重复第一个和第三个顶点完成第二个三角形
    push_sprite_vertex(
        cx + local[0].x * c - local[0].y * s,
        cy + local[0].x * s + local[0].y * c,
        tint, uv[0].x, uv[0].y);
    push_sprite_vertex(
        cx + local[2].x * c - local[2].y * s,
        cy + local[2].x * s + local[2].y * c,
        tint, uv[2].x, uv[2].y);
    push_sprite_vertex(
        cx + local[3].x * c - local[3].y * s,
        cy + local[3].x * s + local[3].y * c,
        tint, uv[3].x, uv[3].y);
}

void VulkanRenderer2D::set_ambient_light(const Color& color) {
    ambient_light_ = color;
}

void VulkanRenderer2D::add_light(const Light2D& light) {
    if (static_cast<int>(lights_.size()) < k_max_lights) {
        lights_.push_back(light);
    }
}

void VulkanRenderer2D::add_point_light(const math::Vector2f& pos, float radius,
                                        const Color& color, float intensity) {
    Light2D light;
    light.type = LightType2D::Point;
    light.position = pos;
    light.radius = radius;
    light.color = color;
    light.intensity = intensity;
    add_light(light);
}

void VulkanRenderer2D::reset_lights() {
    ambient_light_ = Color::black();
    lights_.clear();
}

void VulkanRenderer2D::draw_lit_sprite(float x, float y, float w, float h,
                                        RHITextureHandle albedo, RHITextureHandle normal_map,
                                        const Color& tint) {
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
                           albedo, normal_map, tint,
                           0.0f, 0.0f, 1.0f, 1.0f);
}

void VulkanRenderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                               float u0, float v0, float u1, float v1,
                                               RHITextureHandle albedo, RHITextureHandle normal_map,
                                               const Color& tint,
                                               float nu0, float nv0,
                                               float nu1, float nv1) {
    // albedo 句柄无效回退纯色矩形（与旧裸指针空值行为一致）；
    // normal_map 无效时执行期解析回退默认平面法线
    if (!albedo.is_valid()) {
        draw_rect(x, y, w, h, tint);
        return;
    }

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y0, tint, u1, v0, nu1, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);
    push_lit_vertex(albedo, normal_map, x0, y1, tint, u0, v1, nu0, nv1);
}

void VulkanRenderer2D::draw_shadow_caster(float x, float y, float w, float h) {
    if (!initialized_) return;
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y1);
}

void VulkanRenderer2D::set_bloom(const BloomParams& params) {
    bloom_params_ = params;
    // 同 begin_frame：创建/销毁含直接 vkCreate*/vkDestroy*，
    // 渲染线程运行中需先暂停再操作（对齐 GL 侧）。
    if (bloom_params_.enabled && !bloom_initialized_ && screen_width_ > 0.0f && screen_height_ > 0.0f) {
        if (ctx_->is_running()) {
            ctx_->pause_render_thread();
            create_bloom_targets();
            ctx_->resume_render_thread();
        } else {
            create_bloom_targets();
        }
    }
    if (!bloom_params_.enabled && bloom_initialized_) {
        if (ctx_->is_running()) {
            ctx_->pause_render_thread();
            destroy_bloom_targets();
            ctx_->resume_render_thread();
        } else {
            destroy_bloom_targets();
        }
    }
}

namespace {

// draw_text / measure_text 共用的逐字节推进：两者必须用同一套规则，
// 否则 measure 出来的宽度与实际笔位不一致，彩色 run 会互相压字。
inline float glyph_advance(const FontAtlas& atlas, int bucket, float scale, char c) {
    if (c == '\n' || c == '\r') return -1.0f;
    const Glyph* g = atlas.get_glyph(bucket, static_cast<uint32_t>(static_cast<unsigned char>(c)));
    return g ? g->advance * scale : 0.0f;
}

// 1:1 采样时把字形左上角对齐到像素网格：落在半像素上时双线性会同时混合
// 相邻两行/两列 texel，笔画宽度被抹成 1.5 像素的灰边——观感就是"发虚"。
// 缩放比明显偏离 1 时不再取整（那时亚像素定位的收益更大，取整反而抖字距）。
inline void glyph_quad(float scale, float cursor_x, float cursor_y, const Glyph& g,
                       float* x0, float* y0, float* x1, float* y1) {
    float px = cursor_x + g.offset_x * scale;
    float py = cursor_y + g.offset_y * scale;
    float w = g.width * scale;
    float h = g.height * scale;
    if (std::fabs(scale - 1.0f) < 0.16f) {
        px = std::round(px);
        py = std::round(py);
        w = std::round(w);
        h = std::round(h);
    }
    *x0 = px;
    *y0 = py;
    *x1 = px + w;
    *y1 = py + h;
}

} // namespace

float VulkanRenderer2D::measure_text(const char* text, float font_size) const {
    if (!text || !font_atlas_.texture() || font_size <= 0.0f) return 0.0f;
    const int bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (bucket < 0 || raster <= 0.0f) return 0.0f;
    const float scale = font_size / raster;
    float best = 0.0f;
    float cursor = 0.0f;
    for (const char* p = text; *p; ++p) {
        const float adv = glyph_advance(font_atlas_, bucket, scale, *p);
        if (adv < 0.0f) {
            best = std::max(best, cursor);
            cursor = 0.0f;
            continue;
        }
        cursor += adv;
    }
    return std::max(best, cursor);
}

float VulkanRenderer2D::text_cap_height(float font_size) const {    if (font_size <= 0.0f) return 0.0f;
    if (!font_atlas_.texture()) return font_size * 0.72f;
    const int bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (bucket < 0 || raster <= 0.0f) return font_size * 0.72f;
    return font_atlas_.cap_height(bucket) * (font_size / raster);
}

bool VulkanRenderer2D::ensure_text_raster_size(float px) {
    return ensure_text_raster_sizes(std::vector<float>{px});
}

bool VulkanRenderer2D::ensure_text_raster_sizes(const std::vector<float>& px_list) {
    if (!ctx_ || !font_atlas_.texture() || px_list.empty()) return false;

    // 超采样时图集按 2x 物理像素烘档，整形强度仍要按逻辑字号算（否则 16px 的
    // 字在 32 档上会被当成"大字号"跳过加粗，笔画反而比 1x 更细）。只在 >1 时
    // 设置：非覆盖层调用（3D 文本）不能把它重置回 1，否则每帧来回重烘。
    if (overlay_scale() > 1.0f) {
        FontAtlas::set_shape_logical_scale(overlay_scale());
    }

    std::vector<float> want;
    want.reserve(px_list.size());
    for (float px : px_list) {
        if (px < 6.0f) px = 6.0f;
        if (px > 128.0f) px = 128.0f;
        const float q = std::round(px);
        if (font_atlas_.has_size(q)) continue;
        bool dup = false;
        for (float w : want) {
            if (std::fabs(w - q) < 0.5f) {
                dup = true;
                break;
            }
        }
        if (!dup) want.push_back(q);
    }
    if (want.empty()) {
        // 没有新档，但整形参数可能变过（首次进入超采样），需要按新参数重烘一次
        bool reshaped = false;
        auto bake = [this, &reshaped]() { reshaped = font_atlas_.reshape_if_dirty(ctx_); };
        if (ctx_->is_running()) {
            ctx_->run_with_gpu_context_sync(bake);
        } else {
            bake();
        }
        return reshaped;
    }

    bool baked = false;
    auto bake = [this, &want, &baked]() { baked = font_atlas_.ensure_sizes(ctx_, want); };
    if (ctx_->is_running()) {
        // 重建图集 = 新建纹理 + 整张重传，必须在渲染线程上下文里同步做完：
        // 随后同一帧的 flush_batch 会现取 font_atlas_.texture()，异步会取到旧图。
        ctx_->run_with_gpu_context_sync(bake);
    } else {
        bake();
    }
    if (!baked) return false;

    if (ITexture* tex = font_atlas_.texture()) {
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }
    GLOG_INFO("VulkanRenderer2D: font atlas rebaked at {} size(s) ({} bucket(s))",
              want.size(), font_atlas_.bucket_count());
    return true;
}

float VulkanRenderer2D::text_raster_size() const {
    return font_atlas_.font_size();
}

void VulkanRenderer2D::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {    // 覆盖层超采样：笔位与字号一起换算到离屏目标的分辨率。字号乘上去之后
    // bucket_for 会选到 2x 那一档，字形按离屏像素 1:1 铺开，降采样才有额外的
    // 采样点可平均；只放大坐标不放大字号等于把 1x 位图拉一倍再缩回去。
    if (overlay_scale_ != 1.0f) {
        x *= overlay_scale_;
        y *= overlay_scale_;
        font_size *= overlay_scale_;
    }
    if (!font_atlas_.texture()) {
        float cursor_x = x;
        float cursor_y = y;
        float block_w = font_size * 0.6f;
        float block_h = font_size;
        for (char c : text) {
            if (c == '\n') {
                cursor_x = x;
                cursor_y += block_h;
                continue;
            }
            if (c != ' ') {
                draw_rect(cursor_x, cursor_y - block_h * 0.8f, block_w, block_h, color);
            }
            cursor_x += block_w;
        }
        return;
    }

    // 取**最近的一档**而不是第 0 档：档位是按真实界面字号烘的，缩放比接近 1:1；
    // 固定用第 0 档（初始 32px）等于把 13px 文字缩小 2.4 倍做双线性采样，笔画
    // 边缘全是灰台阶，这就是 Vulkan 文字比 D3D12 糊的原因。
    const int   bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (bucket < 0 || raster <= 0.0f) return;
    const float scale = font_size / raster;
    float cursor_x = x;
    float cursor_y = y;

    for (char c : text) {
        if (c == '\n') {
            cursor_x = x;
            cursor_y += font_size;
            continue;
        }

        const Glyph* g = font_atlas_.get_glyph(bucket, static_cast<uint32_t>(static_cast<unsigned char>(c)));
        if (!g) continue;

        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        glyph_quad(scale, cursor_x, cursor_y, *g, &x0, &y0, &x1, &y1);

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y0, color, g->uv1_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);
        push_text_vertex(x0, y1, color, g->uv0_x, g->uv1_y);

        cursor_x += g->advance * scale;
    }
}

void VulkanRenderer2D::draw_text_colored(float x, float y, float font_size,
                                         const std::vector<ColoredRun>& runs) {
    if (overlay_scale_ != 1.0f) {
        x *= overlay_scale_;
        y *= overlay_scale_;
        font_size *= overlay_scale_;
    }
    if (!font_atlas_.texture()) {
        // 无字体：按色块近似推进，至少保证 run 之间不重叠。
        float cursor_x = x;
        const float block_w = font_size * 0.6f;
        for (const ColoredRun& run : runs) {
            for (char c : run.text) {
                if (c != ' ') {
                    draw_rect(cursor_x, y - font_size * 0.8f, block_w, font_size, run.color);
                }
                cursor_x += block_w;
            }
        }
        return;
    }

    const int   bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (bucket < 0 || raster <= 0.0f) return;
    const float scale = font_size / raster;
    float cursor_x = x;
    for (const ColoredRun& run : runs) {
        for (char c : run.text) {
            if (c == '\n') {
                cursor_x = x;
                continue;
            }
            const Glyph* g = font_atlas_.get_glyph(bucket, static_cast<uint32_t>(static_cast<unsigned char>(c)));
            if (!g) continue;

            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            glyph_quad(scale, cursor_x, y, *g, &x0, &y0, &x1, &y1);

            push_text_vertex(x0, y0, run.color, g->uv0_x, g->uv0_y);
            push_text_vertex(x1, y0, run.color, g->uv1_x, g->uv0_y);
            push_text_vertex(x1, y1, run.color, g->uv1_x, g->uv1_y);

            push_text_vertex(x0, y0, run.color, g->uv0_x, g->uv0_y);
            push_text_vertex(x1, y1, run.color, g->uv1_x, g->uv1_y);
            push_text_vertex(x0, y1, run.color, g->uv0_x, g->uv1_y);

            cursor_x += g->advance * scale;
        }
    }
}

void VulkanRenderer2D::draw_font_atlas_debug(float x, float y, float size) {
    if (!font_atlas_.texture()) return;
    if (overlay_scale_ != 1.0f) {
        x *= overlay_scale_;
        y *= overlay_scale_;
        size *= overlay_scale_;
    }

    Color c = Color::white();
    push_text_vertex(x, y, c, 0.0f, 0.0f);
    push_text_vertex(x + size, y, c, 1.0f, 0.0f);
    push_text_vertex(x + size, y + size, c, 1.0f, 1.0f);

    push_text_vertex(x, y, c, 0.0f, 0.0f);
    push_text_vertex(x + size, y + size, c, 1.0f, 1.0f);
    push_text_vertex(x, y + size, c, 0.0f, 1.0f);
}

RHITextureHandle VulkanRenderer2D::create_texture_from_data(const assets::TextureData* data) {
    if (!ctx_ || !data || data->empty()) return RHITextureHandle{};

    RHITextureHandle handle = ctx_->create_texture();
    if (!handle.is_valid()) return RHITextureHandle{};

    // 上传推到渲染线程执行（对齐 GL 侧）：主线程 staging 上传会
    // 与渲染线程并发 vkQueueSubmit 同一 graphics queue，违反 Vulkan 对
    // queue 的外部同步要求。像素数据按值拷贝，句柄在执行时经 generation
    // 校验解析；绘制命令排在上传命令之后提交，时序有保证。
    auto pixels_copy = std::make_shared<std::vector<unsigned char>>(data->pixels);
    const int width = data->width;
    const int height = data->height;
    const int channels = data->channels;

    ctx_->push_command([this, handle, pixels_copy, width, height, channels](IRenderBackend*) {
        auto* vk_tex = dynamic_cast<VulkanTexture*>(ctx_->texture(handle));
        if (!vk_tex) return;
        vk_tex->upload_data(pixels_copy->data(), width, height, channels);
    });

    return handle;
}

ITexture* VulkanRenderer2D::resolve_texture(RHITextureHandle handle) const {
    return ctx_ ? ctx_->texture(handle) : nullptr;
}

} // namespace gryce_engine::render
