#include "render/ibl_generator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "assets/texture_data.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_two_pi = 2.0f * k_pi;
constexpr float k_half_pi = 0.5f * k_pi;

inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 局部余弦加权半球采样（用于 irradiance 卷积）
struct HemisphereSample {
    float x, y, z;
};

// 基于 Fibonacci sphere 的半球方向集合
std::vector<HemisphereSample> build_hemisphere_samples(int count) {
    std::vector<HemisphereSample> samples;
    samples.reserve(count);
    const float golden_angle = k_pi * (3.0f - std::sqrt(5.0f)); // ~2.39996
    for (int i = 0; i < count; ++i) {
        float y = 1.0f - static_cast<float>(i) / static_cast<float>(count - 1) * 2.0f; // 1 -> -1
        float radius = std::sqrt(1.0f - y * y);
        float theta = golden_angle * static_cast<float>(i);
        samples.push_back({radius * std::cos(theta), y, radius * std::sin(theta)});
    }
    return samples;
}

// 构造以 N 为 Z 轴的切线空间正交基
void build_tangent_basis(const math::Vector3f& N, math::Vector3f& T, math::Vector3f& B) {
    math::Vector3f up = std::fabs(N.y) < 0.999f ? math::Vector3f::up() : math::Vector3f::right();
    T = N.cross(up).normalized();
    B = N.cross(T).normalized();
}

// 从 IBLData 的 cubemap 面中双线性采样
math::Vector3f sample_cubemap_faces(const std::array<std::vector<float>, 6>& faces,
                                    int face_size, const math::Vector3f& dir) {
    if (face_size <= 0) return math::Vector3f::zero();

    // 找到主导轴与面索引
    float ax = std::fabs(dir.x);
    float ay = std::fabs(dir.y);
    float az = std::fabs(dir.z);
    int face = 0;
    float sc = 0.0f, tc = 0.0f, ma = 0.0f;
    if (ax >= ay && ax >= az) {
        face = dir.x > 0.0f ? 0 : 1;
        ma = ax;
        sc = dir.x > 0.0f ? -dir.z : dir.z;
        tc = -dir.y;
    } else if (ay >= ax && ay >= az) {
        face = dir.y > 0.0f ? 2 : 3;
        ma = ay;
        sc = dir.y > 0.0f ? dir.x : dir.x;
        tc = dir.y > 0.0f ? dir.z : -dir.z;
    } else {
        face = dir.z > 0.0f ? 4 : 5;
        ma = az;
        sc = dir.z > 0.0f ? dir.x : -dir.x;
        tc = -dir.y;
    }
    float u = (sc / ma + 1.0f) * 0.5f;
    float v = (tc / ma + 1.0f) * 0.5f;

    u = saturate(u);
    v = saturate(v);

    const auto& fdata = faces[face];
    float fx = u * static_cast<float>(face_size - 1);
    float fy = v * static_cast<float>(face_size - 1);
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    int x1 = std::min(x0 + 1, face_size - 1);
    int y1 = std::min(y0 + 1, face_size - 1);
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto read = [&](int x, int y) -> math::Vector3f {
        size_t idx = (static_cast<size_t>(y) * face_size + x) * 4;
        return math::Vector3f(fdata[idx + 0], fdata[idx + 1], fdata[idx + 2]);
    };

    math::Vector3f c00 = read(x0, y0);
    math::Vector3f c10 = read(x1, y0);
    math::Vector3f c01 = read(x0, y1);
    math::Vector3f c11 = read(x1, y1);
    math::Vector3f c0 = c00 * (1.0f - tx) + c10 * tx;
    math::Vector3f c1 = c01 * (1.0f - tx) + c11 * tx;
    return c0 * (1.0f - ty) + c1 * ty;
}

} // namespace

math::Vector3f IBLGenerator::cubemap_direction(int face, float u, float v) {
    // 将像素中心 [0,1] 映射到 [-1,1]
    float x = u * 2.0f - 1.0f;
    float y = v * 2.0f - 1.0f;
    math::Vector3f dir;
    switch (face) {
        case 0: dir = math::Vector3f( 1.0f, -y, -x); break; // +X
        case 1: dir = math::Vector3f(-1.0f, -y,  x); break; // -X
        case 2: dir = math::Vector3f( x,  1.0f,  y); break; // +Y
        case 3: dir = math::Vector3f( x, -1.0f, -y); break; // -Y
        case 4: dir = math::Vector3f( x, -y,  1.0f); break; // +Z
        case 5: dir = math::Vector3f(-x, -y, -1.0f); break; // -Z
        default: dir = math::Vector3f(0.0f, 0.0f, 1.0f); break;
    }
    return dir.normalized();
}

math::Vector3f IBLGenerator::sample_equirectangular(const assets::TextureData* data,
                                                     const math::Vector3f& dir) {
    if (!data || data->float_pixels.empty()) return math::Vector3f::zero();

    // 方向转球坐标
    float phi = std::atan2(dir.z, dir.x);   // [-pi, pi]
    float theta = std::acos(saturate(dir.y)); // [0, pi]

    float u = (phi / k_two_pi) + 0.5f;
    float v = theta / k_pi;

    // 钳制并采样（双线性）
    int w = data->width;
    int h = data->height;
    if (w <= 0 || h <= 0) return math::Vector3f::zero();

    float fx = u * static_cast<float>(w - 1);
    float fy = v * static_cast<float>(h - 1);
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    int x1 = std::min(x0 + 1, w - 1);
    int y1 = std::min(y0 + 1, h - 1);
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto read = [&](int x, int y) -> math::Vector3f {
        int idx = (y * w + x) * 4;
        return math::Vector3f(data->float_pixels[idx + 0],
                              data->float_pixels[idx + 1],
                              data->float_pixels[idx + 2]);
    };

    math::Vector3f c00 = read(x0, y0);
    math::Vector3f c10 = read(x1, y0);
    math::Vector3f c01 = read(x0, y1);
    math::Vector3f c11 = read(x1, y1);

    math::Vector3f c0 = c00 * (1.0f - tx) + c10 * tx;
    math::Vector3f c1 = c01 * (1.0f - tx) + c11 * tx;
    return c0 * (1.0f - ty) + c1 * ty;
}

void IBLGenerator::convolve_irradiance(const IBLData& src, IBLData& dst) {
    // 注意：生成过程中 prefilter_mips 尚未填充，不能用 src.valid()（会误判失败）
    if (src.cubemap_size <= 0 || src.radiance_faces[0].empty() || dst.irradiance_size <= 0) return;

    const int size = dst.irradiance_size;
    const int samples = 1024;
    auto hemisphere = build_hemisphere_samples(samples);

    for (int face = 0; face < 6; ++face) {
        dst.irradiance_faces[face].resize(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                math::Vector3f N = cubemap_direction(face, u, v);

                math::Vector3f T, B;
                build_tangent_basis(N, T, B);

                math::Vector3f irradiance = math::Vector3f::zero();
                float weight_sum = 0.0f;
                for (const auto& s : hemisphere) {
                    math::Vector3f local(s.x, s.y, s.z);
                    math::Vector3f L = T * local.x + N * local.y + B * local.z;
                    L = L.normalized();
                    float cos_theta = N.dot(L);
                    if (cos_theta <= 0.0f) continue;
                    // 对 radiance cubemap 采样
                    math::Vector3f radiance = sample_cubemap_faces(src.radiance_faces, src.cubemap_size, L);
                    irradiance += radiance * cos_theta;
                    weight_sum += cos_theta;
                }
                if (weight_sum > 0.0f) {
                    irradiance = irradiance * (1.0f / weight_sum);
                }

                size_t idx = (static_cast<size_t>(y) * size + x) * 4;
                dst.irradiance_faces[face][idx + 0] = irradiance.x;
                dst.irradiance_faces[face][idx + 1] = irradiance.y;
                dst.irradiance_faces[face][idx + 2] = irradiance.z;
                dst.irradiance_faces[face][idx + 3] = 1.0f;
            }
        }
    }
}

void IBLGenerator::prefilter_radiance(const IBLData& src, IBLData& dst) {
    // 注意：生成过程中 irradiance/prefilter 尚未填充，不能用 src.valid()
    if (src.cubemap_size <= 0 || src.radiance_faces[0].empty() || dst.prefilter_size <= 0) return;

    const int src_size = src.cubemap_size;
    dst.prefilter_mips.clear();
    dst.prefilter_mips.reserve(IBLData::k_prefilter_mip_levels);

    const float golden_angle = k_pi * (3.0f - std::sqrt(5.0f)); // ~2.39996
    const int levels = IBLData::k_prefilter_mip_levels;

    for (int level = 0; level < levels; ++level) {
        const int size = std::max(1, dst.prefilter_size >> level);
        const float t = static_cast<float>(level) / static_cast<float>(levels - 1);
        // 采样锥半径随 roughness 增大：level 0 接近锐利，末级覆盖大半半球。
        // 用 t*t 让低粗糙度区间更锐利（视觉上更接近真实反射）。
        const float jitter_radius = 0.02f + 0.55f * t * t;
        const int samples = 16 + 48 * level;

        std::array<std::vector<float>, 6> faces;
        for (int face = 0; face < 6; ++face) {
            faces[face].resize(static_cast<size_t>(size) * size * 4);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                    math::Vector3f N = cubemap_direction(face, u, v);

                    math::Vector3f T, B;
                    build_tangent_basis(N, T, B);

                    math::Vector3f color = math::Vector3f::zero();
                    // 黄金角螺旋均匀覆盖圆盘，再投影到半球，近似 GGX 的镜面 lobe。
                    for (int i = 0; i < samples; ++i) {
                        float r = std::sqrt((static_cast<float>(i) + 0.5f) / static_cast<float>(samples));
                        float angle = golden_angle * static_cast<float>(i);
                        float ax = std::cos(angle) * r;
                        float ay = std::sin(angle) * r;
                        math::Vector3f perturbed = (N + T * (ax * jitter_radius) +
                                                    B * (ay * jitter_radius)).normalized();
                        color += sample_cubemap_faces(src.radiance_faces, src_size, perturbed);
                    }
                    color = color * (1.0f / static_cast<float>(samples));

                    size_t idx = (static_cast<size_t>(y) * size + x) * 4;
                    faces[face][idx + 0] = color.x;
                    faces[face][idx + 1] = color.y;
                    faces[face][idx + 2] = color.z;
                    faces[face][idx + 3] = 1.0f;
                }
            }
        }
        dst.prefilter_mips.push_back(std::move(faces));
    }
}

namespace {

// Van der Corput 低差异序列（bit-reversal），配合 Hammersley 点集做重要性采样。
inline float radical_inverse_vdc(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;  // / 2^32
}

// 单点 GGX split-sum 积分贡献：返回 {scale, bias}，对应 shader 里的
// specular = prefiltered * (F0 * scale + bias)。
// 关键点：结果**必须**随 NdotV 变化 —— 掠射角下 Fresnel 项才抬起来；
// 只看粗糙度的近似会让所有表面平白多出一层天空反射（画面像金属）。
inline void ggx_brdf_sample(float NdotV, float alpha, uint32_t i, uint32_t sample_count,
                            float& out_scale, float& out_bias) {
    const float xi1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(sample_count);
    const float xi2 = radical_inverse_vdc(i);

    // 切线空间：N = +Z，V 落在 XZ 平面
    const float Vx = std::sqrt(std::max(0.0f, 1.0f - NdotV * NdotV));
    const float Vz = NdotV;

    // GGX 半程向量重要性采样
    const float phi = k_two_pi * xi1;
    const float alpha2 = alpha * alpha;
    const float cos_theta =
        std::sqrt(std::max(0.0f, (1.0f - xi2) / (1.0f + (alpha2 - 1.0f) * xi2)));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const float Hx = sin_theta * std::cos(phi);
    const float Hy = sin_theta * std::sin(phi);
    const float Hz = cos_theta;

    const float VdotH = Vx * Hx + Vz * Hz;
    if (VdotH <= 0.0f) return;

    // L = reflect(-V, H)
    const float Lx = 2.0f * VdotH * Hx - Vx;
    const float Ly = 2.0f * VdotH * Hy;
    const float Lz = 2.0f * VdotH * Hz - Vz;
    const float NdotL = Lz;
    if (NdotL <= 0.0f) return;

    const float NdotH = Hz > 1.0f ? 1.0f : Hz;
    if (NdotH <= 1.0e-5f) return;

    // Smith 几何项（IBL 用 k = alpha / 2）
    const float k = alpha * 0.5f;
    const float gv = NdotV / (NdotV * (1.0f - k) + k);
    const float gl = NdotL / (NdotL * (1.0f - k) + k);
    const float g_vis = (gv * gl) * VdotH / (NdotH * NdotV);

    const float one_minus_vh = 1.0f - VdotH;
    const float fc = one_minus_vh * one_minus_vh * one_minus_vh * one_minus_vh * one_minus_vh;

    out_scale = (1.0f - fc) * g_vis;
    out_bias = fc * g_vis;
}

} // namespace

void IBLGenerator::integrate_brdf(IBLData& dst, int size) {
    if (size <= 0) return;
    constexpr uint32_t k_sample_count = 256;

    dst.brdf_lut.assign(static_cast<size_t>(size) * size * 2, 0.0f);
    for (int y = 0; y < size; ++y) {
        const float roughness =
            std::max(0.004f, static_cast<float>(y + 0.5f) / static_cast<float>(size));
        const float alpha = roughness * roughness;
        for (int x = 0; x < size; ++x) {
            const float NdotV =
                std::max(1.0e-3f, static_cast<float>(x + 0.5f) / static_cast<float>(size));

            float scale_sum = 0.0f;
            float bias_sum = 0.0f;
            for (uint32_t i = 0; i < k_sample_count; ++i) {
                // ★ 初值必须是 0：ggx_brdf_sample 在 VdotH<=0 / NdotL<=0 时
                //   提前 return，不写输出。若这里初值是 1.0，每个"射到半球外"
                //   的样本都会被当成一次满额镜面加进 A 项 —— 粗糙表面
                //   (r≈0.86) 的 A 因此从 0.52 被抬到 0.95（近 2 倍），
                //   掠射角更夸张，全场景平白多一层天空色高光，看着像金属。
                float s = 0.0f;
                float b = 0.0f;
                ggx_brdf_sample(NdotV, alpha, i, k_sample_count, s, b);
                scale_sum += s;
                bias_sum += b;
            }

            const size_t idx = (static_cast<size_t>(y) * size + x) * 2;
            const float inv = 1.0f / static_cast<float>(k_sample_count);
            dst.brdf_lut[idx + 0] = scale_sum * inv;
            dst.brdf_lut[idx + 1] = bias_sum * inv;
        }
    }
}

std::unique_ptr<IBLData> IBLGenerator::generate(const assets::TextureData* hdr_data,
                                                int radiance_size,
                                                int irradiance_size,
                                                int prefilter_size,
                                                int brdf_size) {
    if (!hdr_data || hdr_data->float_pixels.empty() || hdr_data->width <= 0 || hdr_data->height <= 0) {
        GLOG_ERROR("IBLGenerator: invalid HDR input data");
        return nullptr;
    }

    auto data = std::make_unique<IBLData>();
    data->cubemap_size = radiance_size;
    data->irradiance_size = irradiance_size;
    data->prefilter_size = prefilter_size;
    data->brdf_size = brdf_size;

    // 1. 生成 radiance cubemap
    for (int face = 0; face < 6; ++face) {
        data->radiance_faces[face].resize(static_cast<size_t>(radiance_size) * radiance_size * 4);
        for (int y = 0; y < radiance_size; ++y) {
            for (int x = 0; x < radiance_size; ++x) {
                float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(radiance_size);
                float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(radiance_size);
                math::Vector3f dir = cubemap_direction(face, u, v);
                math::Vector3f c = sample_equirectangular(hdr_data, dir);
                size_t idx = (static_cast<size_t>(y) * radiance_size + x) * 4;
                data->radiance_faces[face][idx + 0] = c.x;
                data->radiance_faces[face][idx + 1] = c.y;
                data->radiance_faces[face][idx + 2] = c.z;
                data->radiance_faces[face][idx + 3] = 1.0f;
            }
        }
    }

    // 2. irradiance
    convolve_irradiance(*data, *data);

    // 3. prefilter
    prefilter_radiance(*data, *data);

    // 4. BRDF LUT
    integrate_brdf(*data, brdf_size);

    GLOG_INFO("IBLGenerator: generated IBL (radiance={}, irradiance={}, prefilter={}, brdf={})",
              radiance_size, irradiance_size, prefilter_size, brdf_size);
    return data;
}

std::unique_ptr<IBLData> IBLGenerator::generate_from_cubemap(
    const std::array<std::vector<float>, 6>& radiance_faces,
    int radiance_size,
    int irradiance_size,
    int prefilter_size,
    int brdf_size) {
    if (radiance_size <= 0 || irradiance_size <= 0 || prefilter_size <= 0 || brdf_size <= 0) {
        GLOG_ERROR("IBLGenerator: invalid cubemap input size");
        return nullptr;
    }
    for (int face = 0; face < 6; ++face) {
        if (radiance_faces[face].size() < static_cast<size_t>(radiance_size) * radiance_size * 4) {
            GLOG_ERROR("IBLGenerator: radiance face {} too small", face);
            return nullptr;
        }
    }

    auto data = std::make_unique<IBLData>();
    data->cubemap_size = radiance_size;
    data->irradiance_size = irradiance_size;
    data->prefilter_size = prefilter_size;
    data->brdf_size = brdf_size;
    data->radiance_faces = radiance_faces;

    convolve_irradiance(*data, *data);
    prefilter_radiance(*data, *data);
    integrate_brdf(*data, brdf_size);

    GLOG_INFO("IBLGenerator: generated IBL from cubemap (radiance={}, irradiance={}, prefilter={}, brdf={})",
              radiance_size, irradiance_size, prefilter_size, brdf_size);
    return data;
}

} // namespace gryce_engine::render
