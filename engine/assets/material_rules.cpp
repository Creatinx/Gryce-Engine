#include "assets/material_rules.h"

#include <algorithm>
#include <cmath>

namespace gryce_engine::assets {

namespace {

constexpr float k_epsilon = 1e-6f;

float clamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}

// 金属度反推的判据阈值（集中放这里，便于日后按素材调整）。
//   漫反射亮度 < k_metal_max_diffuse：金属没有漫反射，Kd 必然接近黑
//   镜面亮度   > k_metal_min_specular：镜面得有能量，否则是纯黑材质
constexpr float k_metal_max_diffuse = 0.06f;
constexpr float k_metal_min_specular = 0.20f;

// 自发光判定阈值：仅用来给 SurfaceType 分类（不影响渲染参数）。
constexpr float k_emissive_threshold = 0.05f;

// 没有粗糙度信息时的兜底：
//   有基色贴图 → 当"扫描/贴图模型"处理，给偏粗糙的值（0.8），避免整片高光；
//   纯色材质 → 引擎默认 0.5。
constexpr float k_default_roughness_textured = 0.80f;
constexpr float k_default_roughness_solid = 0.50f;

} // namespace

const char* surface_type_name(SurfaceType type) {
    switch (type) {
        case SurfaceType::Dielectric: return "Dielectric";
        case SurfaceType::Metal: return "Metal";
        case SurfaceType::Transparent: return "Transparent";
        case SurfaceType::Emissive: return "Emissive";
    }
    return "Unknown";
}

float surface_luminance(const math::Vector3f& color) {
    return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

float shininess_to_roughness(float shininess) {
    if (!(shininess > 0.0f)) {
        // Ns=0 是 MTL 的"无高光"写法，等价于极粗糙
        return 1.0f;
    }
    // α² = 2/(n+2)，r = sqrt(α) = (2/(n+2))^(1/4)
    const float r = std::pow(2.0f / (shininess + 2.0f), 0.25f);
    // 下限 0.04：再光滑的表面在 8bit / IBL 下也留一点高光宽度，否则高光变成噪点
    return std::min(1.0f, std::max(0.04f, r));
}

SurfaceResult resolve_surface(const SurfaceInput& input) {
    SurfaceResult out;
    out.base_color = input.base_color;
    out.emissive = input.emissive;

    // -----------------------------------------------------------------------
    // 1. 粗糙度：PBR 因子 > spec-gloss 高光指数 > 按有无贴图兜底
    // -----------------------------------------------------------------------
    if (input.has_roughness) {
        out.roughness = std::min(1.0f, std::max(0.02f, input.roughness));
    } else if (input.has_shininess) {
        out.roughness = shininess_to_roughness(input.shininess);
    } else {
        out.roughness = input.has_base_color_map ? k_default_roughness_textured
                                                 : k_default_roughness_solid;
    }

    // -----------------------------------------------------------------------
    // 2. 金属度
    // -----------------------------------------------------------------------
    if (input.has_metallic) {
        out.metallic = clamp01(input.metallic);
    } else {
        // spec-gloss 工作流反推：金属没有漫反射，镜面颜色就是它的基色。
        //   * 有基色贴图时 Kd 只是贴图乘子（3ds Max 默认 0.5 灰），不能当依据
        //     —— 否则带贴图的扫描模型全被判成金属（历史事故）；
        //   * 纯色材质才看"漫反射是否接近黑 + 镜面是否有能量"。
        const float diffuse_lum = surface_luminance(input.base_color);
        const float spec_lum = surface_luminance(input.specular_color);
        if (!input.has_base_color_map && input.has_specular &&
            diffuse_lum < k_metal_max_diffuse && spec_lum > k_metal_min_specular) {
            // 金属的 albedo 就是 F0：镜面颜色直接接管基色
            out.base_color = input.specular_color;
            out.metallic = clamp01(spec_lum);
            if (input.has_specular_strength) {
                out.metallic = clamp01(out.metallic * clamp01(input.specular_strength));
            }
        } else {
            out.metallic = 0.0f;
        }
    }

    // -----------------------------------------------------------------------
    // 3. 不透明度 / 双面
    // -----------------------------------------------------------------------
    out.opacity = input.has_opacity ? clamp01(input.opacity) : 1.0f;
    // 阈值 0.999：0.999 这类"文件写了 1.0 但被浮点运算抹了一下"的值不该触发混合
    out.blend = out.opacity < 0.999f;
    out.two_sided = input.two_sided || out.blend;

    // -----------------------------------------------------------------------
    // 4. 表面类型（渲染分支 / 日志分类）
    // -----------------------------------------------------------------------
    if (out.blend) {
        out.type = SurfaceType::Transparent;
    } else if (surface_luminance(out.emissive) > k_emissive_threshold) {
        out.type = SurfaceType::Emissive;
    } else if (out.metallic > 0.5f) {
        out.type = SurfaceType::Metal;
    } else {
        out.type = SurfaceType::Dielectric;
    }

    if (std::abs(out.metallic) < k_epsilon) out.metallic = 0.0f;
    return out;
}

} // namespace gryce_engine::assets
