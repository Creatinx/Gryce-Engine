#include "material.h"

#include <fstream>

#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "render_context.h"
#include "shader.h"
#include "texture.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

void Material::serialize(nlohmann::json& out) const {
    out["name"] = name;
    out["albedo_color"] = { albedo_color.x, albedo_color.y, albedo_color.z };
    out["roughness"] = roughness;
    out["metallic"] = metallic;
    out["ao"] = ao;
    out["emissive_color"] = { emissive_color.x, emissive_color.y, emissive_color.z };
    out["opacity"] = opacity;
    out["blend_mode"] = static_cast<int>(blend_mode);
    out["two_sided"] = two_sided;
    out["uv_scale"] = { uv_scale.x, uv_scale.y };
    out["uv_offset"] = { uv_offset.x, uv_offset.y };
    out["albedo_map_path"] = albedo_map_path;
    out["normal_map_path"] = normal_map_path;
    out["roughness_map_path"] = roughness_map_path;
    out["metallic_map_path"] = metallic_map_path;
    out["ao_map_path"] = ao_map_path;
    out["emissive_map_path"] = emissive_map_path;
    out["use_albedo_map"] = use_albedo_map;
    out["use_normal_map"] = use_normal_map;
    out["use_roughness_map"] = use_roughness_map;
    out["use_metallic_map"] = use_metallic_map;
    out["use_ao_map"] = use_ao_map;
    out["use_emissive_map"] = use_emissive_map;
    out["softness"] = softness;
    out["drag_coefficient"] = drag_coefficient;
    out["density"] = density;
    out["preset_name"] = preset_name;
}

void Material::deserialize(const nlohmann::json& in) {
    name = in.value("name", name);
    auto c = in.value("albedo_color", std::vector<float>{1.0f, 1.0f, 1.0f});
    if (c.size() >= 3) albedo_color = math::Vector3f(c[0], c[1], c[2]);
    roughness = in.value("roughness", roughness);
    metallic = in.value("metallic", metallic);
    ao = in.value("ao", ao);
    auto e = in.value("emissive_color", std::vector<float>{0.0f, 0.0f, 0.0f});
    if (e.size() >= 3) emissive_color = math::Vector3f(e[0], e[1], e[2]);
    opacity = in.value("opacity", opacity);
    blend_mode = static_cast<BlendMode>(in.value("blend_mode", 0));
    two_sided = in.value("two_sided", two_sided);
    auto us = in.value("uv_scale", std::vector<float>{1.0f, 1.0f});
    if (us.size() >= 2) uv_scale = math::Vector2f(us[0], us[1]);
    auto uo = in.value("uv_offset", std::vector<float>{0.0f, 0.0f});
    if (uo.size() >= 2) uv_offset = math::Vector2f(uo[0], uo[1]);
    albedo_map_path = in.value("albedo_map_path", albedo_map_path);
    normal_map_path = in.value("normal_map_path", normal_map_path);
    roughness_map_path = in.value("roughness_map_path", roughness_map_path);
    metallic_map_path = in.value("metallic_map_path", metallic_map_path);
    ao_map_path = in.value("ao_map_path", ao_map_path);
    emissive_map_path = in.value("emissive_map_path", emissive_map_path);
    use_albedo_map = in.value("use_albedo_map", use_albedo_map);
    use_normal_map = in.value("use_normal_map", use_normal_map);
    use_roughness_map = in.value("use_roughness_map", use_roughness_map);
    use_metallic_map = in.value("use_metallic_map", use_metallic_map);
    use_ao_map = in.value("use_ao_map", use_ao_map);
    use_emissive_map = in.value("use_emissive_map", use_emissive_map);
    softness = in.value("softness", softness);
    drag_coefficient = in.value("drag_coefficient", drag_coefficient);
    density = in.value("density", density);
    preset_name = in.value("preset_name", preset_name);
}

bool Material::save_to_file(const std::string& path) const {
    nlohmann::json j;
    serialize(j);
    std::ofstream file(resources::ResourcePath::resolve(path));
    if (!file.is_open()) {
        GLOG_ERROR("Material: failed to save '{}'", path);
        return false;
    }
    file << j.dump(2);
    return true;
}

bool Material::load_from_file(const std::string& path) {
    std::ifstream file(resources::ResourcePath::resolve(path));
    if (!file.is_open()) {
        GLOG_ERROR("Material: failed to open '{}'", path);
        return false;
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& ex) {
        GLOG_ERROR("Material: failed to parse '{}': {}", path, ex.what());
        return false;
    }
    deserialize(j);
    return true;
}

static RHITextureHandle create_fallback_texture(RenderContext* ctx,
                                                 unsigned char r, unsigned char g,
                                                 unsigned char b, unsigned char a) {
    if (!ctx) return RHITextureHandle{};
    RHITextureHandle tex = ctx->create_texture();
    ITexture* tex_ptr = ctx->texture(tex);
    if (!tex.is_valid() || !tex_ptr) return RHITextureHandle{};
    unsigned char pixel[4] = {r, g, b, a};
    tex_ptr->upload_data(pixel, 1, 1, 4);
    return tex;
}

RHITextureHandle Material::load_texture(RenderContext* ctx, const std::string& path) {
    if (path.empty() || !ctx) return RHITextureHandle{};

    // 1. 从资源管线获取 CPU 侧 TextureData（带缓存与引用计数）
    auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(path);
    if (!tex_data.valid()) {
        GLOG_WARN("Material: failed to load texture '{}', using empty fallback", path);
        return create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    // 2. 上传到 GPU
    RHITextureHandle tex = ctx->create_texture();
    ITexture* tex_ptr = ctx->texture(tex);
    if (!tex.is_valid() || !tex_ptr) {
        return create_fallback_texture(ctx, 255, 255, 255, 255);
    }
    if (!tex_ptr->upload_data(tex_data->data(), tex_data->width, tex_data->height, tex_data->channels)) {
        GLOG_WARN("Material: failed to upload texture '{}' to GPU, using fallback", path);
        ctx->destroy_texture(tex);
        return create_fallback_texture(ctx, 255, 255, 255, 255);
    }
    return tex;
}

void Material::upload_to_gpu(RenderContext* ctx) {
    if (!ctx) return;

    // 贴图槽位约定：0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao
    // 每一张图都保证至少有一个 1x1 fallback 绑定到对应 slot，避免 GLSL sampler
    // 在没有纹理绑定的情况下被未定义读取。
    albedo_map_ = load_texture(ctx, albedo_map_path);
    if (!albedo_map_.is_valid()) {
        albedo_map_ = create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    normal_map_ = load_texture(ctx, normal_map_path);
    if (!normal_map_.is_valid()) {
        // 法线贴图默认：纯蓝 (0.5, 0.5, 1.0) 表示无扰动
        normal_map_ = create_fallback_texture(ctx, 128, 128, 255, 255);
    }

    roughness_map_ = load_texture(ctx, roughness_map_path);
    if (!roughness_map_.is_valid()) {
        roughness_map_ = create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    metallic_map_ = load_texture(ctx, metallic_map_path);
    if (!metallic_map_.is_valid()) {
        metallic_map_ = create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    ao_map_ = load_texture(ctx, ao_map_path);
    if (!ao_map_.is_valid()) {
        ao_map_ = create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    emissive_map_ = load_texture(ctx, emissive_map_path);
    if (!emissive_map_.is_valid()) {
        emissive_map_ = create_fallback_texture(ctx, 255, 255, 255, 255);
    }

    GLOG_INFO("Material '{}' uploaded to GPU", name);
}

void Material::bind(RenderContext* ctx, RHIShaderHandle shader) const {
    if (!ctx || !shader.is_valid()) return;

    ctx->set_uniform_vec3(shader, "uAlbedoColor", albedo_color);
    ctx->set_uniform_float(shader, "uRoughness", roughness);
    ctx->set_uniform_float(shader, "uMetallic", metallic);
    ctx->set_uniform_float(shader, "uAO", ao);
    ctx->set_uniform_vec3(shader, "uEmissiveColor", emissive_color);
    ctx->set_uniform_float(shader, "uOpacity", opacity);
    ctx->set_uniform_vec4(shader, "uUVTransform",
                          math::Vector4f(uv_scale.x, uv_scale.y, uv_offset.x, uv_offset.y));

    auto bind_tex = [&](RHITextureHandle tex, int slot, bool use, const char* use_flag,
                        const char* uniform) {
        // 只有当用户启用且确实指定了贴图路径时才采样；否则用标量参数
        bool has = tex.is_valid() && use;
        ctx->set_uniform_int(shader, use_flag, has ? 1 : 0);
        // 无论是否采样都绑定 fallback，保证 sampler 有合法 texture unit
        ctx->set_texture(shader, tex.is_valid() ? tex : albedo_map_, slot, "");
        ctx->set_uniform_int(shader, uniform, slot);
    };

    // 贴图槽位约定：0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 shadow, 6 emissive
    bind_tex(albedo_map_, 0, use_albedo_map && !albedo_map_path.empty(),
             "uUseAlbedoMap", "uAlbedoMap");
    bind_tex(normal_map_, 1, use_normal_map && !normal_map_path.empty(),
             "uUseNormalMap", "uNormalMap");
    bind_tex(roughness_map_, 2, use_roughness_map && !roughness_map_path.empty(),
             "uUseRoughnessMap", "uRoughnessMap");
    bind_tex(metallic_map_, 3, use_metallic_map && !metallic_map_path.empty(),
             "uUseMetallicMap", "uMetallicMap");
    bind_tex(ao_map_, 4, use_ao_map && !ao_map_path.empty(),
             "uUseAOMap", "uAOMap");
    bind_tex(emissive_map_, 6, use_emissive_map && !emissive_map_path.empty(),
             "uUseEmissiveMap", "uEmissiveMap");
}

void Material::destroy_gpu(RenderContext* ctx) {
    if (!ctx) return;
    auto destroy = [&](RHITextureHandle& tex) {
        if (tex.is_valid()) {
            ctx->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    };
    destroy(albedo_map_);
    destroy(normal_map_);
    destroy(roughness_map_);
    destroy(metallic_map_);
    destroy(ao_map_);
    destroy(emissive_map_);
}

} // namespace gryce_engine::render
