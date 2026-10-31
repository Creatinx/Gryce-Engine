// Surface Nets 等值面提取：把规则标量场转成水密三角网格。
// 供 SPHFluid3D（粒子密度场）与 FluidDomain3D（FLIP 液体稠密场、欧拉气体密度场）
// 共用，避免同一套拓扑逻辑存在多份实现。

#include "components/3d/surface_nets.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gryce_engine::components::surface_nets {

namespace {

// 单元 → 顶点下标的临时缓冲。提取只在渲染线程的收集阶段串行调用，
// 用 thread_local 复用，避免每帧为 64³ 场反复分配十几 MB。
thread_local std::vector<int> g_cell_vertex;

// 12 条边的端点（角点编号 c = x + 2y + 4z）
constexpr int k_edge[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   // along x
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along y
    {0, 4}, {1, 5}, {2, 6}, {3, 7}    // along z
};

} // namespace

float sample_trilinear(const float* field, int nx, int ny, int nz, float gx, float gy, float gz) {
    const int nx1 = nx + 1;
    const int ny1 = ny + 1;
    const auto at = [&](int x, int y, int z) {
        x = std::clamp(x, 0, nx);
        y = std::clamp(y, 0, ny);
        z = std::clamp(z, 0, nz);
        return field[static_cast<size_t>((z * ny1 + y) * nx1 + x)];
    };

    gx = std::clamp(gx, 0.0f, static_cast<float>(nx));
    gy = std::clamp(gy, 0.0f, static_cast<float>(ny));
    gz = std::clamp(gz, 0.0f, static_cast<float>(nz));
    const int x0 = std::min(static_cast<int>(gx), nx - 1);
    const int y0 = std::min(static_cast<int>(gy), ny - 1);
    const int z0 = std::min(static_cast<int>(gz), nz - 1);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);
    const float fz = gz - static_cast<float>(z0);

    const float c00 = at(x0, y0, z0) + (at(x0 + 1, y0, z0) - at(x0, y0, z0)) * fx;
    const float c10 = at(x0, y0 + 1, z0) + (at(x0 + 1, y0 + 1, z0) - at(x0, y0 + 1, z0)) * fx;
    const float c01 = at(x0, y0, z0 + 1) + (at(x0 + 1, y0, z0 + 1) - at(x0, y0, z0 + 1)) * fx;
    const float c11 =
        at(x0, y0 + 1, z0 + 1) + (at(x0 + 1, y0 + 1, z0 + 1) - at(x0, y0 + 1, z0 + 1)) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

math::Vector3f gradient(const float* field, int nx, int ny, int nz, float gx, float gy, float gz) {
    constexpr float k_step = 0.5f;
    const float dx = sample_trilinear(field, nx, ny, nz, gx + k_step, gy, gz) -
                     sample_trilinear(field, nx, ny, nz, gx - k_step, gy, gz);
    const float dy = sample_trilinear(field, nx, ny, nz, gx, gy + k_step, gz) -
                     sample_trilinear(field, nx, ny, nz, gx, gy - k_step, gz);
    const float dz = sample_trilinear(field, nx, ny, nz, gx, gy, gz + k_step) -
                     sample_trilinear(field, nx, ny, nz, gx, gy, gz - k_step);
    return math::Vector3f(dx, dy, dz);
}

void extract(const float* field, int nx, int ny, int nz, const math::Vector3f& origin, float cell,
             float iso, assets::MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    if (!field || nx <= 0 || ny <= 0 || nz <= 0) return;

    const int nx1 = nx + 1;
    const int ny1 = ny + 1;
    const auto at = [&](int x, int y, int z) {
        return field[static_cast<size_t>((z * ny1 + y) * nx1 + x)];
    };

    const size_t cell_count = static_cast<size_t>(nx) * static_cast<size_t>(ny) *
                              static_cast<size_t>(nz);
    g_cell_vertex.assign(cell_count, -1);

    out.vertices.reserve(static_cast<size_t>(nx) * 4);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                float v[8];
                bool inside = false;
                bool outside = false;
                for (int c = 0; c < 8; ++c) {
                    v[c] = at(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
                    if (v[c] > iso) inside = true;
                    else outside = true;
                }
                if (!inside || !outside) continue;

                math::Vector3f acc = math::Vector3f::zero();
                int crossings = 0;
                for (int e = 0; e < 12; ++e) {
                    const int a = k_edge[e][0];
                    const int b = k_edge[e][1];
                    if ((v[a] > iso) == (v[b] > iso)) continue;
                    const float t = (iso - v[a]) / (v[b] - v[a]);
                    const math::Vector3f pa(static_cast<float>(a & 1),
                                            static_cast<float>((a >> 1) & 1),
                                            static_cast<float>((a >> 2) & 1));
                    const math::Vector3f pb(static_cast<float>(b & 1),
                                            static_cast<float>((b >> 1) & 1),
                                            static_cast<float>((b >> 2) & 1));
                    acc = acc + (pa + (pb - pa) * t);
                    ++crossings;
                }
                if (crossings == 0) continue;

                const math::Vector3f local = acc * (1.0f / static_cast<float>(crossings));
                const float vx = static_cast<float>(x) + local.x;
                const float vy = static_cast<float>(y) + local.y;
                const float vz = static_cast<float>(z) + local.z;

                assets::MeshVertex vert;
                vert.position = math::Vector3f(origin.x + vx * cell, origin.y + vy * cell,
                                               origin.z + vz * cell);
                const math::Vector3f g = gradient(field, nx, ny, nz, vx, vy, vz);
                const float gl = g.length();
                vert.normal = (gl > 1e-9f) ? (g * (-1.0f / gl)) : math::Vector3f(0.0f, 1.0f, 0.0f);
                vert.tangent = math::Vector3f::right();
                vert.uv = math::Vector2f(vert.position.x, vert.position.z);
                vert.color = math::Vector3f(1.0f, 1.0f, 1.0f);

                g_cell_vertex[static_cast<size_t>((z * ny + y) * nx + x)] =
                    static_cast<int>(out.vertices.size());
                out.vertices.push_back(vert);
            }
        }
    }

    if (out.vertices.empty()) return;

    const auto cell_at = [&](int x, int y, int z) {
        return g_cell_vertex[static_cast<size_t>((z * ny + y) * nx + x)];
    };
    const auto emit_quad = [&](int a, int b, int c, int d) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        out.indices.push_back(static_cast<uint32_t>(a));
        out.indices.push_back(static_cast<uint32_t>(b));
        out.indices.push_back(static_cast<uint32_t>(c));
        out.indices.push_back(static_cast<uint32_t>(a));
        out.indices.push_back(static_cast<uint32_t>(c));
        out.indices.push_back(static_cast<uint32_t>(d));
    };

    for (int z = 1; z < nz; ++z) {
        for (int y = 1; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                if ((at(x, y, z) > iso) != (at(x + 1, y, z) > iso)) {
                    emit_quad(cell_at(x, y - 1, z - 1), cell_at(x, y, z - 1), cell_at(x, y, z),
                              cell_at(x, y - 1, z));
                }
            }
        }
    }
    for (int z = 1; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 1; x < nx; ++x) {
                if ((at(x, y, z) > iso) != (at(x, y + 1, z) > iso)) {
                    emit_quad(cell_at(x - 1, y, z - 1), cell_at(x - 1, y, z), cell_at(x, y, z),
                              cell_at(x, y, z - 1));
                }
            }
        }
    }
    for (int z = 0; z < nz; ++z) {
        for (int y = 1; y < ny; ++y) {
            for (int x = 1; x < nx; ++x) {
                if ((at(x, y, z) > iso) != (at(x, y, z + 1) > iso)) {
                    emit_quad(cell_at(x - 1, y - 1, z), cell_at(x, y - 1, z), cell_at(x, y, z),
                              cell_at(x - 1, y, z));
                }
            }
        }
    }

    // 统一绕序：四边形的朝向取决于等值面方向，逐条边生成的顺序必然一半反一半正。
    // two_sided 材质按 gl_FrontFacing 翻法线，绕序不一致会让一半面被翻成背面。
    for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        const uint32_t i0 = out.indices[t];
        const uint32_t i1 = out.indices[t + 1];
        const uint32_t i2 = out.indices[t + 2];
        const math::Vector3f& p0 = out.vertices[i0].position;
        const math::Vector3f& p1 = out.vertices[i1].position;
        const math::Vector3f& p2 = out.vertices[i2].position;
        const math::Vector3f u = p1 - p0;
        const math::Vector3f w = p2 - p0;
        const math::Vector3f face(u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z,
                                  u.x * w.y - u.y * w.x);
        const math::Vector3f nref =
            out.vertices[i0].normal + out.vertices[i1].normal + out.vertices[i2].normal;
        if (face.x * nref.x + face.y * nref.y + face.z * nref.z < 0.0f) {
            out.indices[t + 1] = i2;
            out.indices[t + 2] = i1;
        }
    }
}

} // namespace gryce_engine::components::surface_nets
