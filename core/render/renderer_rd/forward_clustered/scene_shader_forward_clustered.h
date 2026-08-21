#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// 场景 Shader 变体键
// 使用位掩码编码不同的 shader 变体组合。
// 类似于 Godot 的 SceneShaderForwardClustered::VariantKey。
// ---------------------------------------------------------------------------
enum SceneShaderVariant : uint32_t {
    SHADER_VARIANT_BASE             = 0,         // 基础 PBR
    SHADER_VARIANT_NORMAL_MAP       = 1 << 0,    // 法线贴图
    SHADER_VARIANT_SHADOW           = 1 << 1,    // 阴影
    SHADER_VARIANT_LIGHTMAP         = 1 << 2,    // Lightmap
    SHADER_VARIANT_SKINNED          = 1 << 3,    // 蒙皮
    SHADER_VARIANT_MOTION_VECTORS   = 1 << 4,    // 运动向量
    SHADER_VARIANT_DEPTH_NORMAL     = 1 << 5,    // 深度+法线 prepass
    SHADER_VARIANT_TWO_SIDED        = 1 << 6,    // 双面渲染
    SHADER_VARIANT_TRANSPARENT      = 1 << 7,    // 透明
    SHADER_VARIANT_ADVANCED         = 1 << 8,    // 高级（sss/clearcoat/anisotropy）
};

// ---------------------------------------------------------------------------
// SceneShaderForwardClustered — 场景 PBR Shader 变体管理
// 管理所有 Forward Clustered 渲染所需的 shader 变体。
// 类似于 Godot 的 SceneShaderForwardClustered。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API SceneShaderForwardClustered {
public:
    enum ShaderGroup {
        SHADER_GROUP_BASE,              // 基础 PBR 渲染
        SHADER_GROUP_ADVANCED,          // 法线/粗糙度/高光分离
        SHADER_GROUP_MULTIVIEW,         // 多视图（XR）
        SHADER_GROUP_MOTION_VECTORS,    // 运动向量
        SHADER_GROUP_DEPTH_PREPASS,     // Depth Prepass
        SHADER_GROUP_MAX
    };

    SceneShaderForwardClustered();
    ~SceneShaderForwardClustered();

    // 初始化 shader 系统
    bool init(RenderContext* ctx, const std::string& shader_dir);
    void shutdown();

    // 获取指定变体的 shader 句柄
    RHIShaderHandle get_shader(uint32_t variant_key, ShaderGroup group = SHADER_GROUP_BASE);

    // 启用/禁用 shader group
    void enable_group(ShaderGroup group);
    void disable_group(ShaderGroup group);
    bool is_group_enabled(ShaderGroup group) const;

    // 热重载
    bool hot_reload();
    int poll_hot_reload(RenderContext& ctx);

    // 默认 shader 路径
    static constexpr const char* k_default_shader_path = "res:/shaders/forward_clustered";

private:
    // 生成 shader 变体
    RHIShaderHandle _compile_variant(uint32_t variant_key, ShaderGroup group);
    std::string _variant_key_to_string(uint32_t variant_key) const;
    std::string _group_to_shader_name(ShaderGroup group) const;

    RenderContext* ctx_ = nullptr;
    bool initialized_ = false;

    // 变体缓存：key -> shader handle
    struct ShaderVariant {
        RHIShaderHandle handle;
        bool compiled = false;
    };
    std::unordered_map<uint32_t, ShaderVariant> variants_[SHADER_GROUP_MAX];

    // 启用的 group
    uint32_t enabled_groups_ = 0;

    // shader 目录
    std::string shader_dir_;
};

} // namespace gryce_engine::render