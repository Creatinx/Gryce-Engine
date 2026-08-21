#include "render/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "render/render_context.h"
#include "render/shader.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

#include <sstream>

namespace gryce_engine::render {

SceneShaderForwardClustered::SceneShaderForwardClustered() = default;

SceneShaderForwardClustered::~SceneShaderForwardClustered() {
    shutdown();
}

bool SceneShaderForwardClustered::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;
    shader_dir_ = shader_dir;
    initialized_ = true;
    return true;
}

void SceneShaderForwardClustered::shutdown() {
    if (initialized_) {
        for (int g = 0; g < SHADER_GROUP_MAX; ++g) {
            for (auto& [key, variant] : variants_[g]) {
                if (variant.compiled) {
                    ctx_->destroy_shader(variant.handle);
                }
            }
            variants_[g].clear();
        }
        initialized_ = false;
    }
}

std::string SceneShaderForwardClustered::_group_to_shader_name(ShaderGroup group) const {
    switch (group) {
        case SHADER_GROUP_BASE:            return "scene_pbr_ssbo";
        case SHADER_GROUP_ADVANCED:        return "scene_pbr_advanced";
        case SHADER_GROUP_MULTIVIEW:       return "scene_pbr_multiview";
        case SHADER_GROUP_MOTION_VECTORS:  return "scene_motion_vectors";
        case SHADER_GROUP_DEPTH_PREPASS:   return "scene_depth_prepass";
        default:                           return "scene_pbr";
    }
}

std::string SceneShaderForwardClustered::_variant_key_to_string(uint32_t key) const {
    std::string flags;
    if (key & SHADER_VARIANT_NORMAL_MAP)     flags += "_normal";
    if (key & SHADER_VARIANT_SHADOW)         flags += "_shadow";
    if (key & SHADER_VARIANT_LIGHTMAP)       flags += "_lightmap";
    if (key & SHADER_VARIANT_SKINNED)        flags += "_skinned";
    if (key & SHADER_VARIANT_MOTION_VECTORS) flags += "_motion";
    if (key & SHADER_VARIANT_DEPTH_NORMAL)   flags += "_depth_normal";
    if (key & SHADER_VARIANT_TWO_SIDED)      flags += "_twosided";
    if (key & SHADER_VARIANT_TRANSPARENT)    flags += "_transparent";
    if (key & SHADER_VARIANT_ADVANCED)       flags += "_advanced";
    return flags;
}

void SceneShaderForwardClustered::enable_group(ShaderGroup group) {
    enabled_groups_ |= (1 << group);
}

void SceneShaderForwardClustered::disable_group(ShaderGroup group) {
    enabled_groups_ &= ~(1 << group);
}

bool SceneShaderForwardClustered::is_group_enabled(ShaderGroup group) const {
    return (enabled_groups_ & (1 << group)) != 0;
}

RHIShaderHandle SceneShaderForwardClustered::get_shader(uint32_t variant_key, ShaderGroup group) {
    auto it = variants_[group].find(variant_key);
    if (it != variants_[group].end() && it->second.compiled) {
        return it->second.handle;
    }

    // 编译新变体
    return _compile_variant(variant_key, group);
}

RHIShaderHandle SceneShaderForwardClustered::_compile_variant(uint32_t variant_key, ShaderGroup group) {
    std::string shader_name = _group_to_shader_name(group);
    std::string variant_str = _variant_key_to_string(variant_key);

    // 构造 shader 路径描述（仅用于日志）
    std::string shader_desc = shader_dir_ + "/" + shader_name + variant_str;

    // 创建 shader handle
    RHIShaderHandle handle = ctx_->create_shader();
    if (!handle.is_valid()) {
        GLOG_ERROR("SceneShaderForwardClustered: failed to create shader: {}", shader_desc);
        return {};
    }

    // 获取 IShader 对象
    IShader* shader_obj = ctx_->shader(handle);
    if (!shader_obj) {
        GLOG_ERROR("SceneShaderForwardClustered: failed to get shader object: {}", shader_desc);
        ctx_->destroy_shader(handle);
        return {};
    }

    // 使用 load_program 加载 .vert/.frag shader 源文件
    // load_program 会在 shader_dir_ 目录下查找 shader_name.vert 和 shader_name.frag
    if (!shader_obj->load_program(shader_name, shader_dir_)) {
        GLOG_ERROR("SceneShaderForwardClustered: failed to load shader program '{}' from '{}'",
                   shader_name, shader_dir_);
        ctx_->destroy_shader(handle);
        return {};
    }

    if (!shader_obj->is_valid()) {
        GLOG_ERROR("SceneShaderForwardClustered: shader program not valid: {}", shader_desc);
        ctx_->destroy_shader(handle);
        return {};
    }

    // 缓存变体
    ShaderVariant& variant = variants_[group][variant_key];
    variant.handle = handle;
    variant.compiled = true;

    GLOG_INFO("SceneShaderForwardClustered: compiled variant '{}' (key={})", shader_desc, variant_key);
    return handle;
}

bool SceneShaderForwardClustered::hot_reload() {
    // 清除编译标志，下次 get_shader 会重新编译
    for (int g = 0; g < SHADER_GROUP_MAX; ++g) {
        for (auto& [key, variant] : variants_[g]) {
            variant.compiled = false;
        }
    }
    return true;
}

int SceneShaderForwardClustered::poll_hot_reload(RenderContext& ctx) {
    // 简化：检查所有变体是否需要重载
    // 实际实现应使用文件修改时间轮询
    return 0;
}

} // namespace gryce_engine::render