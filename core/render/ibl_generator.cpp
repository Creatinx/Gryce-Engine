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
    if (!src.valid() || dst.irradiance_size <= 0) return;

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
    if (!src.valid() || dst.prefilter_size <= 0) return;

    // 基础版 prefilter：对 radiance 做简单 box blur（模拟中等粗糙度）。
    // 升级方案：为 cubemap 增加 mipmap 后，按 roughness 采样不同 mip。
    const int size = dst.prefilter_size;
    const int src_size = src.cubemap_size;
    const int blur_radius = std::max(1, src_size / (size * 4));

    for (int face = 0; face < 6; ++face) {
        dst.prefilter_faces[face].resize(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                math::Vector3f N = cubemap_direction(face, u, v);

                math::Vector3f color = math::Vector3f::zero();
                float weight = 0.0f;
                int samples = 16;
                // 在 N 周围小范围抖动采样，避免高频噪声
                for (int i = 0; i < samples; ++i) {
                    float angle = static_cast<float>(i) / static_cast<float>(samples) * k_two_pi;
                    float radius = static_cast<float>(i) / static_cast<float>(samples);
                    float ax = std::cos(angle) * radius;
                    float ay = std::sin(angle) * radius;
                    // 构造一个偏离 N 的方向
                    math::Vector3f T, B;
                    build_tangent_basis(N, T, B);
                    math::Vector3f perturbed = (N + T * ax * 0.15f + B * ay * 0.15f).normalized();
                    color += sample_cubemap_faces(src.radiance_faces, src_size, perturbed);
                    weight += 1.0f;
                }
                color = color * (1.0f / weight);

                size_t idx = (static_cast<size_t>(y) * size + x) * 4;
                dst.prefilter_faces[face][idx + 0] = color.x;
                dst.prefilter_faces[face][idx + 1] = color.y;
                dst.prefilter_faces[face][idx + 2] = color.z;
                dst.prefilter_faces[face][idx + 3] = 1.0f;
            }
        }
    }
}

void IBLGenerator::integrate_brdf(IBLData& dst, int size) {
    dst.brdf_lut.resize(static_cast<size_t>(size) * size * 2);
    for (int y = 0; y < size; ++y) {
        float roughness = std::max(0.004f, static_cast<float>(y + 0.5f) / static_cast<float>(size));
        for (int x = 0; x < size; ++x) {
            float NdotV = static_cast<float>(x + 0.5f) / static_cast<float>(size);
            // 解析近似 BRDF 积分（基于 Disney 的 split-sum 近似）
            // 输出 scale (x) 和 bias (y)，供 shader 组合 Fresnel。
            float r4 = roughness * roughness * roughness * roughness;
            float scale = (-1.0f / (r4 + 4.0f) + 1.0f);
            float bias = r4 / (r4 + 4.0f);
            size_t idx = (static_cast<size_t>(y) * size + x) * 2;
            dst.brdf_lut[idx + 0] = scale;
            dst.brdf_lut[idx + 1] = bias;
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

} // namespace gryce_engine::render
