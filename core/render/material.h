#pragma once

#include <string>

#include "math/math.h"
#include "render/rhi_handle.h"
#include <nlohmann/json.hpp>

namespace gryce_engine {
namespace render {

class IShader;
class ITexture;
class RenderContext;

// ---------------------------------------------------------------------------
// Material — 渲染材质
// 支持 PBR 工作流：albedo、normal、roughness、metallic、ao 贴图 + 标量参数
// ---------------------------------------------------------------------------
class Material {
public:
    std::string name = "Default";

    // 基础颜色（无 albedo 贴图时使用）
    math::Vector3f albedo_color = math::Vector3f::one();
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ao = 1.0f;

    // 贴图路径（res:/path 或绝对路径）
    std::string albedo_map_path;
    std::string normal_map_path;
    std::string roughness_map_path;
    std::string metallic_map_path;
    std::string ao_map_path;

    // 是否使用各通道贴图
    bool use_albedo_map = true;
    bool use_normal_map = true;
    bool use_roughness_map = true;
    bool use_metallic_map = true;
    bool use_ao_map = true;

    Material() = default;
    explicit Material(const std::string& material_name) : name(material_name) {}

    // 物理材质属性（用于 ImGui 显示与物理计算）
    float softness = 0.0f;          // 软度 0~1，0=硬，1=软
    float drag_coefficient = 0.0f;  // 风阻/阻力系数 0~1
    float density = 1.0f;           // 密度（相对单位）
    std::string preset_name;        // 当前使用的预设名称

    // 在 RenderContext::start() 之前调用（主线程持有 GL context）
    void upload_to_gpu(RenderContext* ctx);

    // 绑定材质到 shader（将渲染命令 push 到 RenderContext）
    void bind(RenderContext* ctx, RHIShaderHandle shader) const;

    // 释放 GPU 纹理（push 到渲染线程）
    void destroy_gpu(RenderContext* ctx);

    bool has_gpu_textures() const {
        return albedo_map_.is_valid() || normal_map_.is_valid() || roughness_map_.is_valid() ||
               metallic_map_.is_valid() || ao_map_.is_valid();
    }

    void serialize(nlohmann::json& out) const;
    void deserialize(const nlohmann::json& in);

    // GPU 句柄（由 upload_to_gpu 填充）
    RHITextureHandle albedo_texture() const { return albedo_map_; }
    RHITextureHandle normal_texture() const { return normal_map_; }
    RHITextureHandle roughness_texture() const { return roughness_map_; }
    RHITextureHandle metallic_texture() const { return metallic_map_; }
    RHITextureHandle ao_texture() const { return ao_map_; }

private:
    RHITextureHandle load_texture(RenderContext* ctx, const std::string& path);

    RHITextureHandle albedo_map_;
    RHITextureHandle normal_map_;
    RHITextureHandle roughness_map_;
    RHITextureHandle metallic_map_;
    RHITextureHandle ao_map_;
};

} // namespace render
} // namespace gryce_engine
