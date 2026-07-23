#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "math/math.h"

namespace gryce_engine {
namespace assets { struct TextureData; }
namespace render {

// ---------------------------------------------------------------------------
// IBLGenerator — 从 equirectangular HDR/EXR 环境贴图生成 IBL 所需资源
//
// 输出：
//   - radiance_cubemap：环境辐射度 cubemap（RGBA16F，radiance 面）
//   - irradiance_cubemap：漫反射 irradiance cubemap（RGBA16F，CPU 卷积）
//   - prefilter_cubemap：用于镜面反射的预过滤 cubemap（RGBA16F，基础版）
//   - brdf_lut：BRDF 积分 LUT（RG16F，2D）
//
// 说明：
//   - 当前为 IBL 基础实现。prefilter 暂使用单级 cubemap（无 mipmap），
//     shader 中用 roughness 做简单衰减；cubemap mipmap 生成完成后可升级为
//     标准 split-sum approximation。
// ---------------------------------------------------------------------------
struct IBLData {
    int cubemap_size = 0;
    int irradiance_size = 0;
    int prefilter_size = 0;
    int brdf_size = 0;

    // 每个面数据：顺序 +X, -X, +Y, -Y, +Z, -Z
    std::array<std::vector<float>, 6> radiance_faces;
    std::array<std::vector<float>, 6> irradiance_faces;
    std::array<std::vector<float>, 6> prefilter_faces;

    // BRDF LUT：RG 各一个 float，按行优先存储
    std::vector<float> brdf_lut;

    bool valid() const {
        return cubemap_size > 0 && irradiance_size > 0 && prefilter_size > 0 && brdf_size > 0;
    }
};

class IBLGenerator {
public:
    // 从 equirectangular HDR/EXR TextureData 生成 IBL 资源。
    // radiance_size：环境 cubemap 边长（建议 512 或 256）
    // irradiance_size：irradiance cubemap 边长（建议 32）
    // prefilter_size：prefilter cubemap 边长（建议 128）
    // brdf_size：BRDF LUT 边长（建议 256）
    static std::unique_ptr<IBLData> generate(const assets::TextureData* hdr_data,
                                             int radiance_size = 512,
                                             int irradiance_size = 32,
                                             int prefilter_size = 128,
                                             int brdf_size = 256);

private:
    static math::Vector3f cubemap_direction(int face, float u, float v);
    static math::Vector3f sample_equirectangular(const assets::TextureData* data,
                                                  const math::Vector3f& dir);
    static void convolve_irradiance(const IBLData& src, IBLData& dst);
    static void prefilter_radiance(const IBLData& src, IBLData& dst);
    static void integrate_brdf(IBLData& dst, int size);
};

} // namespace render
} // namespace gryce_engine
