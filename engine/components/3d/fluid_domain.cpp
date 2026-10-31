// FluidDomain3D — Mantaflow 式流体域：FLIP 液体与欧拉气体共用一条 MAC 速度场。

#include "components/3d/fluid_domain.h"

#include <algorithm>
#include <cmath>

#include "components/3d/surface_nets.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/render_context.h"

namespace gryce_engine::components {

namespace {

constexpr float k_fixed_dt = 1.0f / 60.0f;
constexpr int k_max_substeps = 2;
constexpr int k_min_res = 8;
constexpr int k_max_res = 96;
// 空气格的判定阈值：液体体积分数低于它、且烟雾稀薄，该格压力置零
// （自由表面 Dirichlet）。取 0.5 是 VOF 的标准分界面。
constexpr float k_air_frac = 0.5f;
// 浮力换算成平流速度的比例（m/s 每单位密度）。直接取 gas_buoyancy 当加速度用
// 会让回溯距离大到跨越好几个格子，采样严重失真，这里给一个比例系数。
constexpr float k_buoyancy_advect = 0.55f;

// 样本坐标空间里的三线性采样：sx/sy/sz 是三个方向的样本个数。
// x/y/z 是样本坐标（整数下标对应样本位置），越界按端点夹取。
float tri_sample(const std::vector<float>& f, int sx, int sy, int sz, float x, float y, float z) {
    if (sx <= 0 || sy <= 0 || sz <= 0) return 0.0f;
    x = std::clamp(x, 0.0f, static_cast<float>(sx - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(sy - 1));
    z = std::clamp(z, 0.0f, static_cast<float>(sz - 1));
    const int x0 = std::min(static_cast<int>(x), std::max(0, sx - 2));
    const int y0 = std::min(static_cast<int>(y), std::max(0, sy - 2));
    const int z0 = std::min(static_cast<int>(z), std::max(0, sz - 2));
    const int x1 = std::min(x0 + 1, sx - 1);
    const int y1 = std::min(y0 + 1, sy - 1);
    const int z1 = std::min(z0 + 1, sz - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float fz = z - static_cast<float>(z0);

    const auto at = [&](int xi, int yi, int zi) {
        return f[static_cast<size_t>((zi * sy + yi) * sx + xi)];
    };
    const float c00 = at(x0, y0, z0) + (at(x1, y0, z0) - at(x0, y0, z0)) * fx;
    const float c10 = at(x0, y1, z0) + (at(x1, y1, z0) - at(x0, y1, z0)) * fx;
    const float c01 = at(x0, y0, z1) + (at(x1, y0, z1) - at(x0, y0, z1)) * fx;
    const float c11 = at(x0, y1, z1) + (at(x1, y1, z1) - at(x0, y1, z1)) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

FluidDomain3D::FluidDomain3D() {
    setup_grid();
    reset_fluid();
}

FluidDomain3D::~FluidDomain3D() { alive_token_->store(false, std::memory_order_release); }

void FluidDomain3D::setup_grid() {
    const int res = std::clamp(resolution, k_min_res, k_max_res);
    const float max_ext = std::max(domain_extents.x,
                                   std::max(domain_extents.y, domain_extents.z)) * 2.0f;
    grid_.cell = std::max(1e-4f, max_ext / static_cast<float>(res));
    grid_.origin = math::Vector3f(-domain_extents.x, -domain_extents.y, -domain_extents.z);
    for (int a = 0; a < 3; ++a) {
        const float extent = (&domain_extents.x)[a] * 2.0f;
        grid_.dim[a] = std::max(1, static_cast<int>(std::lround(extent / grid_.cell)));
    }
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];

    grid_.u.assign(static_cast<size_t>(nx + 1) * ny * nz, 0.0f);
    grid_.v.assign(static_cast<size_t>(nx) * (ny + 1) * nz, 0.0f);
    grid_.w.assign(static_cast<size_t>(nx) * ny * (nz + 1), 0.0f);
    grid_.prev_u.assign(grid_.u.size(), 0.0f);
    grid_.prev_v.assign(grid_.v.size(), 0.0f);
    grid_.prev_w.assign(grid_.w.size(), 0.0f);
    const size_t cells = static_cast<size_t>(nx) * ny * nz;
    grid_.p.assign(cells, 0.0f);
    grid_.div.assign(cells, 0.0f);
    grid_.frac.assign(cells, 0.0f);
    grid_.density.assign(cells, 0.0f);
    grid_.tmp.assign(cells, 0.0f);
    grid_.p_next.assign(cells, 0.0f);
    grid_.curl_x.assign(cells, 0.0f);
    grid_.curl_y.assign(cells, 0.0f);
    grid_.curl_z.assign(cells, 0.0f);
    grid_.adv_u.assign(grid_.u.size(), 0.0f);
    grid_.adv_v.assign(grid_.v.size(), 0.0f);
    grid_.adv_w.assign(grid_.w.size(), 0.0f);
}

void FluidDomain3D::reset_fluid() {
    setup_grid();
    positions_.clear();
    velocities_.clear();
    std::fill(grid_.frac.begin(), grid_.frac.end(), 0.0f);
    std::fill(grid_.density.begin(), grid_.density.end(), 0.0f);
    std::fill(grid_.p.begin(), grid_.p.end(), 0.0f);
    std::fill(grid_.u.begin(), grid_.u.end(), 0.0f);
    std::fill(grid_.v.begin(), grid_.v.end(), 0.0f);
    std::fill(grid_.w.begin(), grid_.w.end(), 0.0f);

    if (!liquid_enabled) return;

    // 出生：在液池盒内按 0.62 格距播种，贴住域的底面。间距比格距小，
    // 保证初始体积分数过 0.5、压力投影一开始就能形成连续水面。
    //
    // 粒子上限不能靠"播到上限就停"来实现：那样会沿播种顺序（x 最快、z 最慢）
    // 截断，水只剩一道窄条。这里先按目标间距算总数，超限就把间距按体积比
    // 放大再算，让布局始终是完整的一片。
    const math::Vector3f half = liquid_spawn_extents;
    const int limit = std::max(256, max_particles);
    float spacing = grid_.cell * 0.62f;
    int nx = 1;
    int ny = 1;
    int nz = 1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        nx = std::max(1, static_cast<int>((half.x * 2.0f) / spacing));
        ny = std::max(1, static_cast<int>((half.y * 2.0f) / spacing));
        nz = std::max(1, static_cast<int>((half.z * 2.0f) / spacing));
        const long long total = static_cast<long long>(nx) * ny * nz;
        if (total <= limit) break;
        spacing *= std::cbrt(static_cast<float>(total) / static_cast<float>(limit)) * 1.02f;
    }
    spacing_ = spacing;
    const math::Vector3f base(-half.x, -domain_extents.y + spacing * 0.5f, -half.z);
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (static_cast<int>(positions_.size()) >= limit) break;
                positions_.push_back(math::Vector3f(base.x + static_cast<float>(i) * spacing,
                                                    base.y + static_cast<float>(j) * spacing,
                                                    base.z + static_cast<float>(k) * spacing));
            }
        }
    }
    velocities_.assign(positions_.size(), math::Vector3f::zero());
}

void FluidDomain3D::on_update(float dt) {
    if (!simulate) return;
    if (positions_.empty() && liquid_enabled) reset_fluid();

    accumulator_ += std::clamp(dt, 0.0f, 0.1f);
    int steps = 0;
    while (accumulator_ >= k_fixed_dt && steps < k_max_substeps) {
        simulate_step(k_fixed_dt);
        accumulator_ -= k_fixed_dt;
        ++steps;
    }
    if (steps == k_max_substeps) accumulator_ = 0.0f;
}

void FluidDomain3D::on_destroy() { invalidate_gpu(); }

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

void FluidDomain3D::serialize(nlohmann::json& out) const {
    out["domain_extents"] = {domain_extents.x, domain_extents.y, domain_extents.z};
    out["resolution"] = resolution;
    out["liquid_enabled"] = liquid_enabled;
    out["liquid_spawn_extents"] = {liquid_spawn_extents.x, liquid_spawn_extents.y,
                                   liquid_spawn_extents.z};
    out["liquid_spawn_fill"] = liquid_spawn_fill;
    out["max_particles"] = max_particles;
    out["flip_ratio"] = flip_ratio;
    out["water_color"] = {water_color.x, water_color.y, water_color.z, water_color.w};
    out["water_roughness"] = water_roughness;
    out["water_metallic"] = water_metallic;
    out["liquid_iso"] = liquid_iso;
    out["gas_enabled"] = gas_enabled;
    out["gas_source_center"] = {gas_source_center.x, gas_source_center.y, gas_source_center.z};
    out["gas_source_extents"] = {gas_source_extents.x, gas_source_extents.y, gas_source_extents.z};
    out["gas_emission"] = gas_emission;
    out["gas_buoyancy"] = gas_buoyancy;
    out["gas_drag"] = gas_drag;
    out["gas_dissipation"] = gas_dissipation;
    out["gas_vorticity"] = gas_vorticity;
    out["smoke_color"] = {smoke_color.x, smoke_color.y, smoke_color.z, smoke_color.w};
    out["smoke_shells"] = smoke_shells;
    out["smoke_iso"] = smoke_iso;
    out["gravity"] = {gravity.x, gravity.y, gravity.z};
    out["wind"] = {wind.x, wind.y, wind.z};
    out["pressure_iterations"] = pressure_iterations;
    out["surface_update_interval"] = surface_update_interval;
    out["simulate"] = simulate;
}

void FluidDomain3D::deserialize(const nlohmann::json& in) {
    const auto read_vec3 = [&](const char* key, math::Vector3f& dst) {
        if (in.contains(key) && in[key].is_array() && in[key].size() == 3) {
            const auto& a = in[key];
            dst = math::Vector3f(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>());
        }
    };
    const auto read_vec4 = [&](const char* key, math::Vector4f& dst) {
        if (in.contains(key) && in[key].is_array() && in[key].size() == 4) {
            const auto& a = in[key];
            dst = math::Vector4f(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>(),
                                 a.at(3).get<float>());
        }
    };
    const auto read_f = [&](const char* key, float& dst) {
        if (in.contains(key) && in[key].is_number()) dst = in[key].get<float>();
    };
    const auto read_i = [&](const char* key, int& dst) {
        if (in.contains(key) && in[key].is_number_integer()) dst = in[key].get<int>();
    };
    const auto read_b = [&](const char* key, bool& dst) {
        if (in.contains(key) && in[key].is_boolean()) dst = in[key].get<bool>();
    };

    read_vec3("domain_extents", domain_extents);
    read_i("resolution", resolution);
    read_b("liquid_enabled", liquid_enabled);
    read_vec3("liquid_spawn_extents", liquid_spawn_extents);
    read_f("liquid_spawn_fill", liquid_spawn_fill);
    read_i("max_particles", max_particles);
    read_f("flip_ratio", flip_ratio);
    read_vec4("water_color", water_color);
    read_f("water_roughness", water_roughness);
    read_f("water_metallic", water_metallic);
    read_f("liquid_iso", liquid_iso);
    read_b("gas_enabled", gas_enabled);
    read_vec3("gas_source_center", gas_source_center);
    read_vec3("gas_source_extents", gas_source_extents);
    read_f("gas_emission", gas_emission);
    read_f("gas_buoyancy", gas_buoyancy);
    read_f("gas_drag", gas_drag);
    read_f("gas_dissipation", gas_dissipation);
    read_f("gas_vorticity", gas_vorticity);
    read_vec4("smoke_color", smoke_color);
    read_i("smoke_shells", smoke_shells);
    read_f("smoke_iso", smoke_iso);
    read_vec3("gravity", gravity);
    read_vec3("wind", wind);
    read_i("pressure_iterations", pressure_iterations);
    read_i("surface_update_interval", surface_update_interval);
    read_b("simulate", simulate);

    initialized_ = false;
    layers_.clear();
    reset_fluid();
}

// ---------------------------------------------------------------------------
// 模拟
// ---------------------------------------------------------------------------

bool FluidDomain3D::in_domain(const math::Vector3f& p) const {
    return p.x > -domain_extents.x && p.x < domain_extents.x && p.y > -domain_extents.y &&
           p.y < domain_extents.y && p.z > -domain_extents.z && p.z < domain_extents.z;
}

// 粒子积分只有重力和外部风：FLIP 不需要压力力，压力是通过速度场投影体现的。
void FluidDomain3D::integrate_particles(float dt) {
    const math::Vector3f g = gravity * dt;
    for (size_t i = 0; i < positions_.size(); ++i) {
        math::Vector3f& vel = velocities_[i];
        vel = vel + g;
        // 空气拖曳：粒子上采样到的空气速度会把它往当地流场带，这是"空气也是
        // 流体"在粒子侧的直接体现（风吹水面起波纹）。
        //
        // 只取**水平分量**、按当地烟雾密度加权、并且很弱。竖直分量绝不能用：
        // 上升的烟柱在下侧伴随很强的回流，把粒子往下吸、水就从中间被掏空、
        // 摊成一圈围着池壁。
        if (gas_enabled) {
            const math::Vector3f gd(positions_[i].x - grid_.origin.x,
                                    positions_[i].y - grid_.origin.y,
                                    positions_[i].z - grid_.origin.z);
            const float gx = gd.x / grid_.cell;
            const float gy = gd.y / grid_.cell;
            const float gz = gd.z / grid_.cell;
            const float d = sample_cell_scalar(grid_.density, gx, gy, gz);
            if (d > 1e-3f) {
                const math::Vector3f air = sample_cell_velocity(gx, gy, gz);
                const float k = std::min(0.5f, 0.25f * dt * d);
                vel.x += (air.x - vel.x) * k;
                vel.z += (air.z - vel.z) * k;
            }
        }
        positions_[i] = positions_[i] + vel * dt;
    }
}

// P2G：粒子速度按三线性权重散播到三种面心，同时统计格心体积分数。
//
// 只重建**液体**的面心速度。P2G 必须先把三种分量清零再累加（否则平均被上一
// 步的残留污染），但清零同时会把气相速度一起抹掉：空气是欧拉量，靠速度场自
// 平流与浮力累积动量，每步被 P2G 清一次就永远涨不起来 —— 表现为浮力把密度往
// 上推一点点、竖直速度却始终停在单步增量上，烟只会堆在源口。
// 所以清零前先留一份快照，散播完再把"两侧都是空气"的面心恢复成快照值。
void FluidDomain3D::particles_to_grid() {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const size_t cells = static_cast<size_t>(nx) * ny * nz;

    // 值累加器借 adv_*（P2G 阶段不需要快照）：液体只**覆盖**有粒子贡献的面，
    // 没有粒子贡献的面保留原值 —— 那是气相的速度场，清零等于每步抹掉空气的
    // 动量，浮力就永远攒不起来。
    grid_.adv_u.assign(grid_.u.size(), 0.0f);
    grid_.adv_v.assign(grid_.v.size(), 0.0f);
    grid_.adv_w.assign(grid_.w.size(), 0.0f);
    grid_.tmp.assign(grid_.u.size() + grid_.v.size() + grid_.w.size(), 0.0f);
    float* wu = grid_.tmp.data();
    float* wv = wu + grid_.u.size();
    float* ww = wv + grid_.v.size();
    std::vector<float> frac_acc(cells, 0.0f);

    const float h = grid_.cell;
    for (size_t p = 0; p < positions_.size(); ++p) {
        const math::Vector3f& pos = positions_[p];
        const math::Vector3f g((pos.x - grid_.origin.x) / h, (pos.y - grid_.origin.y) / h,
                               (pos.z - grid_.origin.z) / h);

        // 三种分量各自的样本坐标：u 在 (i, j+0.5, k+0.5)，以此类推
        const auto splat = [&](std::vector<float>& dst, float* acc, int base_x, int base_y,
                               int base_z, int sx, int sy, int sz, float sc_x, float sc_y,
                               float sc_z, float value, int comp) {
            const float fx0 = sc_x - static_cast<float>(base_x);
            const float fy0 = sc_y - static_cast<float>(base_y);
            const float fz0 = sc_z - static_cast<float>(base_z);
            (void)sx;
            (void)sy;
            for (int dk = 0; dk < 2; ++dk) {
                const int zi = base_z + dk;
                if (zi < 0 || zi >= sz) continue;
                const float wz = dk ? fz0 : (1.0f - fz0);
                for (int dj = 0; dj < 2; ++dj) {
                    const int yi = base_y + dj;
                    if (yi < 0 || yi >= sy) continue;
                    const float wy = dj ? fy0 : (1.0f - fy0);
                    for (int di = 0; di < 2; ++di) {
                        const int xi = base_x + di;
                        if (xi < 0 || xi >= sx) continue;
                        const float wx = di ? fx0 : (1.0f - fx0);
                        const float weight = wx * wy * wz;
                        if (weight <= 0.0f) continue;
                        const size_t index = static_cast<size_t>((zi * sy + yi) * sx + xi);
                        dst[index] += weight * value;
                        acc[index] += weight;
                    }
                }
            }
            (void)comp;
        };

        const int ux = static_cast<int>(std::floor(g.x));
        const int uy = static_cast<int>(std::floor(g.y - 0.5f));
        const int uz = static_cast<int>(std::floor(g.z - 0.5f));
        splat(grid_.adv_u, wu, ux, uy, uz, nx + 1, ny, nz, g.x, g.y - 0.5f, g.z - 0.5f,
              velocities_[p].x, 0);

        const int vx = static_cast<int>(std::floor(g.x - 0.5f));
        const int vy = static_cast<int>(std::floor(g.y));
        const int vz = static_cast<int>(std::floor(g.z - 0.5f));
        splat(grid_.adv_v, wv, vx, vy, vz, nx, ny + 1, nz, g.x - 0.5f, g.y, g.z - 0.5f,
              velocities_[p].y, 1);

        const int wx = static_cast<int>(std::floor(g.x - 0.5f));
        const int wy = static_cast<int>(std::floor(g.y - 0.5f));
        const int wz = static_cast<int>(std::floor(g.z));
        splat(grid_.adv_w, ww, wx, wy, wz, nx, ny, nz + 1, g.x - 0.5f, g.y - 0.5f, g.z,
              velocities_[p].z, 2);

        // 格心体积分数：按最近 8 个格心的三线性权重累加
        const int cx = static_cast<int>(std::floor(g.x - 0.5f));
        const int cy = static_cast<int>(std::floor(g.y - 0.5f));
        const int cz = static_cast<int>(std::floor(g.z - 0.5f));
        const float fx = (g.x - 0.5f) - static_cast<float>(cx);
        const float fy = (g.y - 0.5f) - static_cast<float>(cy);
        const float fz = (g.z - 0.5f) - static_cast<float>(cz);
        for (int dk = 0; dk < 2; ++dk) {
            const int zi = cz + dk;
            if (zi < 0 || zi >= nz) continue;
            for (int dj = 0; dj < 2; ++dj) {
                const int yi = cy + dj;
                if (yi < 0 || yi >= ny) continue;
                for (int di = 0; di < 2; ++di) {
                    const int xi = cx + di;
                    if (xi < 0 || xi >= nx) continue;
                    const float weight = (di ? fx : 1.0f - fx) * (dj ? fy : 1.0f - fy) *
                                         (dk ? fz : 1.0f - fz);
                    if (weight <= 0.0f) continue;
                    frac_acc[idx_c(xi, yi, zi)] += weight;
                }
            }
        }
    }

    // 每格期望粒子数 = (格距 / 播种间距)^3。必须按实际间距算：粒子上限会把
    // spacing 放大，用固定常数的话整片水的 frac 会系统性偏低，投影把它们
    // 全当空气（p = 0），水就失去压力支撑、一层层被压到池底。
    const float per_cell = std::max(
        0.5f, (grid_.cell / std::max(1e-4f, spacing_)) * (grid_.cell / std::max(1e-4f, spacing_)) *
                  (grid_.cell / std::max(1e-4f, spacing_)));
    for (size_t i = 0; i < cells; ++i) {
        grid_.frac[i] = std::clamp(frac_acc[i] / per_cell, 0.0f, 1.0f);
    }

    // 写回面心速度。没有粒子贡献的面分成两类：
    //   ① 远离液体（离最近的液体格还有两格以上）—— 保留原值，那是气相自己的
    //      速度场，清零等于每步抹掉空气的动量，浮力永远攒不起来；
    //   ② 贴着液体 —— 清零，交给压力投影去补。这类面绝不能留旧值：投影的散度
    //      正是拿它们算的，留着上一帧的气相速度会让液体格凭空多出散度，水面
    //      被掏空、摊成一圈围着池壁。
    // "靠近液体"用一次膨胀把液体格扩一圈预先算好，避免逐面查 26 邻域。
    constexpr float k_empty_frac = 0.1f;
    for (size_t i = 0; i < cells; ++i) {
        grid_.p_next[i] = (grid_.frac[i] >= k_empty_frac) ? 1.0f : 0.0f;
    }
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                if (grid_.p_next[c] > 0.5f || grid_.frac[c] >= k_empty_frac) continue;
                const auto liquid_at = [&](int xx, int yy, int zz) {
                    if (xx < 0 || xx >= nx || yy < 0 || yy >= ny || zz < 0 || zz >= nz)
                        return false;
                    return grid_.frac[idx_c(xx, yy, zz)] >= k_empty_frac;
                };
                if (liquid_at(x - 1, y, z) || liquid_at(x + 1, y, z) || liquid_at(x, y - 1, z) ||
                    liquid_at(x, y + 1, z) || liquid_at(x, y, z - 1) || liquid_at(x, y, z + 1)) {
                    grid_.p_next[c] = 1.0f;
                }
            }
        }
    }
    const auto near_liquid = [&](int x, int y, int z) {
        return grid_.p_next[idx_c(std::clamp(x, 0, nx - 1), std::clamp(y, 0, ny - 1),
                                  std::clamp(z, 0, nz - 1))] > 0.5f;
    };
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x <= nx; ++x) {
                const size_t i = static_cast<size_t>(idx_u(x, y, z));
                if (wu[i] > 1e-6f) grid_.u[i] = grid_.adv_u[i] / wu[i];
                else if (near_liquid(x - 1, y, z) || near_liquid(x, y, z)) grid_.u[i] = 0.0f;
            }
        }
    }
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y <= ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(idx_v(x, y, z));
                if (wv[i] > 1e-6f) grid_.v[i] = grid_.adv_v[i] / wv[i];
                else if (near_liquid(x, y - 1, z) || near_liquid(x, y, z)) grid_.v[i] = 0.0f;
            }
        }
    }
    for (int z = 0; z <= nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(idx_w(x, y, z));
                if (ww[i] > 1e-6f) grid_.w[i] = grid_.adv_w[i] / ww[i];
                else if (near_liquid(x, y, z - 1) || near_liquid(x, y, z)) grid_.w[i] = 0.0f;
            }
        }
    }

    // 把液体速度外推一层到"贴水但没有粒子贡献"的面心。
    // G2P 的 PIC 分量（1 - flip_ratio）采样的是速度场的**绝对值**，这些面如果
    // 留 0 或留气相速度，水面粒子每步都会被掺进一份不属于水的速度（8% 权重 ×
    // 气相几 m/s），几百步下来整池水就被搅成一圈围在池壁。外推一层后，水面附近
    // 采样到的都是液相速度。
    const auto fill_from_neighbors = [&](std::vector<float>& field, float* mask, int axis) {
        const int sx = (axis == 0) ? nx + 1 : nx;
        const int sy = (axis == 1) ? ny + 1 : ny;
        const int sz = (axis == 2) ? nz + 1 : nz;
        std::vector<float> next = field;
        for (int z = 0; z < sz; ++z) {
            for (int y = 0; y < sy; ++y) {
                for (int x = 0; x < sx; ++x) {
                    const size_t i = static_cast<size_t>((z * sy + y) * sx + x);
                    if (mask[i] > 1e-6f) continue;
                    int has_liquid = 0;
                    if (axis == 0) has_liquid = near_liquid(x - 1, y, z) || near_liquid(x, y, z);
                    if (axis == 1) has_liquid = near_liquid(x, y - 1, z) || near_liquid(x, y, z);
                    if (axis == 2) has_liquid = near_liquid(x, y, z - 1) || near_liquid(x, y, z);
                    if (!has_liquid) continue;
                    float sum = 0.0f;
                    int count = 0;
                    const auto take = [&](int xx, int yy, int zz) {
                        if (xx < 0 || xx >= sx || yy < 0 || yy >= sy || zz < 0 || zz >= sz) return;
                        const size_t j = static_cast<size_t>((zz * sy + yy) * sx + xx);
                        if (mask[j] <= 1e-6f) return;
                        sum += field[j];
                        ++count;
                    };
                    if (axis == 0) { take(x - 1, y, z); take(x + 1, y, z); }
                    if (axis == 1) { take(x, y - 1, z); take(x, y + 1, z); }
                    if (axis == 2) { take(x, y, z - 1); take(x, y, z + 1); }
                    if (count == 0) continue;
                    next[i] = sum / static_cast<float>(count);
                    mask[i] = 2e-6f;   // 标记已填，下一轮可继续外推
                }
            }
        }
        field = next;
    };
    fill_from_neighbors(grid_.u, wu, 0);
    fill_from_neighbors(grid_.v, wv, 1);
    fill_from_neighbors(grid_.w, ww, 2);
}

// 采样任意格坐标处的速度（三种 MAC 分量分别三线性插值后合成矢量）。
math::Vector3f FluidDomain3D::sample_cell_velocity(float gx, float gy, float gz) const {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float u = tri_sample(grid_.u, nx + 1, ny, nz, gx, gy - 0.5f, gz - 0.5f);
    const float v = tri_sample(grid_.v, nx, ny + 1, nz, gx - 0.5f, gy, gz - 0.5f);
    const float w = tri_sample(grid_.w, nx, ny, nz + 1, gx - 0.5f, gy - 0.5f, gz);
    return math::Vector3f(u, v, w);
}

float FluidDomain3D::sample_cell_scalar(const std::vector<float>& field, float gx, float gy,
                                        float gz) const {
    return tri_sample(field, grid_.dim[0], grid_.dim[1], grid_.dim[2], gx - 0.5f, gy - 0.5f,
                      gz - 0.5f);
}

// 气源：入流边界（Blender 的 Inflow）。源盒内的密度直接给到 gas_emission，
// 而不是每步"加一点"。
//
// 加法的稳态密度 = 每步注入量 / 每步被冲走的比例。上升流在源区每步会替换掉
// 三成以上的格子内容，注入量换算下来只剩 0.12 左右 —— 永远低于任何合理的
// 等值面阈值，烟就只剩一层若有若无的薄壳。入流边界给的是"流进去的东西有多
// 浓"，与流速无关，源区密度稳定在 gas_emission，烟柱才有实体。
void FluidDomain3D::emit_gas(float dt) {
    (void)dt;
    if (!gas_enabled || gas_emission <= 0.0f) return;
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float h = grid_.cell;
    const float level = std::min(1.6f, gas_emission);
    // 入流不做成严格定常。定常入流在数值上是稳定的层流，长出来永远是一根笔
    // 直的柱子；真实烟羽靠源头的扰动才会起卷、分叉成团。这里给每个源格一份
    // 随步变化的白噪声，浮力把它放大成不稳定性。
    const auto hash01 = [](int x, int y, int z, int t) {
        unsigned hsh = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u ^
                       static_cast<unsigned>(z) * 83492791u ^ static_cast<unsigned>(t) * 2654435761u;
        hsh ^= hsh >> 13;
        hsh *= 1274126177u;
        hsh ^= hsh >> 16;
        return static_cast<float>(hsh & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const math::Vector3f c(grid_.origin.x + (static_cast<float>(x) + 0.5f) * h,
                                       grid_.origin.y + (static_cast<float>(y) + 0.5f) * h,
                                       grid_.origin.z + (static_cast<float>(z) + 0.5f) * h);
                if (std::abs(c.x - gas_source_center.x) > gas_source_extents.x) continue;
                if (std::abs(c.y - gas_source_center.y) > gas_source_extents.y) continue;
                if (std::abs(c.z - gas_source_center.z) > gas_source_extents.z) continue;
                const size_t i = static_cast<size_t>(idx_c(x, y, z));
                const float jitter = 0.65f + 0.7f * hash01(x, y, z, step_index_);
                grid_.density[i] = std::max(grid_.density[i], level * jitter);
            }
        }
    }
}

// 浮力：密度越大越往上浮。v 分量在 y 面心，取上下两格密度的平均。
// 同时按密度给一份阻尼：只有浮力没有阻力时竖直速度会逐帧累加到十几 m/s，
// 回溯距离超过好几格，平流采样严重失真；加了阻尼速度收敛到 buoyancy/drag。
//
// 只在"面两侧都是空气"处加力。贴着液体的面归粒子所有，往那里加浮力等于让
// 烟把水池整体往上抬，水面会被冲成一层薄壳再撕碎。
void FluidDomain3D::apply_buoyancy(float dt) {
    if (!gas_enabled) return;
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float drag = std::max(0.0f, gas_drag);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny + 1; ++y) {
            for (int x = 0; x < nx; ++x) {
                // 两侧格（y-1 / y）都必须不在液体里
                const bool air0 = (y > 0) ? grid_.frac[idx_c(x, y - 1, z)] < k_air_frac : true;
                const bool air1 = (y < ny) ? grid_.frac[idx_c(x, y, z)] < k_air_frac : true;
                if (!air0 || !air1) continue;
                const float d0 = (y > 0) ? grid_.density[idx_c(x, y - 1, z)] : 0.0f;
                const float d1 = (y < ny) ? grid_.density[idx_c(x, y, z)] : 0.0f;
                const float d = 0.5f * (d0 + d1);
                if (d <= 1e-4f) continue;
                float& v = grid_.v[idx_v(x, y, z)];
                v += (gas_buoyancy - drag * v) * d * dt;
            }
        }
    }
}

// 外部风写进空气格：液体格里的风会被压力投影立刻抵消（那里要满足不可压）。
void FluidDomain3D::apply_wind(float dt) {
    if (wind.length_sq() < 1e-8f) return;
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float blend = std::min(1.0f, 6.0f * dt);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                if (grid_.frac[c] >= k_air_frac) continue;
                if (x < nx) {
                    float& u = grid_.u[idx_u(x, y, z)];
                    u += (wind.x - u) * blend;
                }
                // 竖直分量不参与风的收敛：烟柱靠浮力上升，风只会把竖直速度
                // 往 0 拉，烟就永远起不来。
                (void)0;
                if (z < nz) {
                    float& w = grid_.w[idx_w(x, y, z)];
                    w += (wind.z - w) * blend;
                }
            }
        }
    }
}

// 涡量约束：ω = ∇×u，把 ω 的模长梯度方向与 ω 的叉积作为力加回速度场。
// 半拉格朗日平流 + 压力的数值耗散会把小涡抹平，没有这一步烟只会慢慢摊成
// 一片均匀的雾，不会翻滚。
//
// 只作用于**空气格**：液体格/液气交界面上的面心速度归 FLIP 粒子所有，往里
// 加涡量力等于给水面凭空注入加速度，水池会被撕成丝带（烟源贴着水面时尤其
// 明显）。
void FluidDomain3D::apply_vorticity(float dt) {
    if (!gas_enabled || gas_vorticity <= 0.0f) return;
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float h = grid_.cell;
    const auto is_air_cell = [&](int x, int y, int z) {
        return grid_.frac[idx_c(x, y, z)] < k_air_frac;
    };

    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                if (grid_.density[c] < 1e-3f || !is_air_cell(x, y, z)) {
                    grid_.curl_x[c] = grid_.curl_y[c] = grid_.curl_z[c] = 0.0f;
                    continue;
                }
                const auto vel = [&](int xx, int yy, int zz) {
                    xx = std::clamp(xx, 0, nx - 1);
                    yy = std::clamp(yy, 0, ny - 1);
                    zz = std::clamp(zz, 0, nz - 1);
                    return sample_cell_velocity(static_cast<float>(xx) + 0.5f,
                                                static_cast<float>(yy) + 0.5f,
                                                static_cast<float>(zz) + 0.5f);
                };
                const math::Vector3f vxp = vel(x + 1, y, z);
                const math::Vector3f vxm = vel(x - 1, y, z);
                const math::Vector3f vyp = vel(x, y + 1, z);
                const math::Vector3f vym = vel(x, y - 1, z);
                const math::Vector3f vzp = vel(x, y, z + 1);
                const math::Vector3f vzm = vel(x, y, z - 1);
                const float inv = 1.0f / (2.0f * h);
                grid_.curl_x[c] = ((vyp.z - vym.z) - (vzp.y - vzm.y)) * inv;
                grid_.curl_y[c] = ((vzp.x - vzm.x) - (vxp.z - vxm.z)) * inv;
                grid_.curl_z[c] = ((vxp.y - vxm.y) - (vyp.x - vym.x)) * inv;
            }
        }
    }

    for (int z = 1; z < nz - 1; ++z) {
        for (int y = 1; y < ny - 1; ++y) {
            for (int x = 1; x < nx - 1; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                const size_t cx1 = static_cast<size_t>(idx_c(x + 1, y, z));
                const size_t cx0 = static_cast<size_t>(idx_c(x - 1, y, z));
                const size_t cy1 = static_cast<size_t>(idx_c(x, y + 1, z));
                const size_t cy0 = static_cast<size_t>(idx_c(x, y - 1, z));
                const size_t cz1 = static_cast<size_t>(idx_c(x, y, z + 1));
                const size_t cz0 = static_cast<size_t>(idx_c(x, y, z - 1));
                const auto mag = [&](size_t i) {
                    return std::sqrt(grid_.curl_x[i] * grid_.curl_x[i] +
                                     grid_.curl_y[i] * grid_.curl_y[i] +
                                     grid_.curl_z[i] * grid_.curl_z[i]);
                };
                const math::Vector3f grad((mag(cx1) - mag(cx0)) / (2.0f * h),
                                          (mag(cy1) - mag(cy0)) / (2.0f * h),
                                          (mag(cz1) - mag(cz0)) / (2.0f * h));
                const float glen = grad.length();
                if (glen < 1e-5f) continue;
                const math::Vector3f nrm = grad * (1.0f / glen);
                const math::Vector3f omega(grid_.curl_x[c], grid_.curl_y[c], grid_.curl_z[c]);
                const math::Vector3f force = nrm.cross(omega) * (gas_vorticity * h);
                // 格心的力摊回三种面心（相邻两格取平均）。只有面两侧都是空气才写：
                // 沾到液体的面心归粒子，动它就是动水面。
                if (x > 0 && is_air_cell(x - 1, y, z) && is_air_cell(x, y, z))
                    grid_.u[idx_u(x, y, z)] += force.x * dt * 0.5f;
                if (x + 1 < nx && is_air_cell(x, y, z) && is_air_cell(x + 1, y, z))
                    grid_.u[idx_u(x + 1, y, z)] += force.x * dt * 0.5f;
                if (y > 0 && is_air_cell(x, y - 1, z) && is_air_cell(x, y, z))
                    grid_.v[idx_v(x, y, z)] += force.y * dt * 0.5f;
                if (y + 1 < ny && is_air_cell(x, y, z) && is_air_cell(x, y + 1, z))
                    grid_.v[idx_v(x, y + 1, z)] += force.y * dt * 0.5f;
                if (z > 0 && is_air_cell(x, y, z - 1) && is_air_cell(x, y, z))
                    grid_.w[idx_w(x, y, z)] += force.z * dt * 0.5f;
                if (z + 1 < nz && is_air_cell(x, y, z) && is_air_cell(x, y, z + 1))
                    grid_.w[idx_w(x, y, z + 1)] += force.z * dt * 0.5f;
            }
        }
    }
}

// 速度场自平流：半拉格朗日回溯，把速度场按它自己搬运一遍（u·∇u 项）。
//
// 这一步是"烟会卷"和"烟是一根柱子"的分水岭。上升的烟柱中间快、两侧慢，本来
// 就有强剪切，涡量约束也确实在放大这份涡量 —— 但速度场自己不被自己搬运的话，
// 涡始终待在被放大的那一格里，不会往下游卷、不会把周围的空气拖进来，最终
// 只表现为一根越来越快的直柱。Blender / Mantaflow 里烟雾的翻滚几乎全部来自
// 这一项。
//
// 三个分量各自在**自己的面心**回溯：u 在 (x, y+0.5, z+0.5)，v 在 (x+0.5, y,
// z+0.5)，w 在 (x+0.5, y+0.5, z)。采样一律读快照 adv_*，边写边读会让同一帧
// 内的结果依赖于遍历顺序。
//
// 只对空气格生效：液体格的速度由粒子 P2G 每步重建，在那里再做一次自平流
// 等于把动量平流两次，白白引入耗散、削掉水花。权重按 frac 线性衰减，
// 面两侧任一格是液体就不动该面。
void FluidDomain3D::advect_velocity(float dt) {
    if (!gas_enabled) return;
    if (dt <= 0.0f) return;

    grid_.adv_u = grid_.u;
    grid_.adv_v = grid_.v;
    grid_.adv_w = grid_.w;

    // axis: 0 = u（面在 x 上），1 = v，2 = w
    advect_component(grid_.u, grid_.adv_u, grid_.dim[0] + 1, grid_.dim[1], grid_.dim[2], 0, dt);
    advect_component(grid_.v, grid_.adv_v, grid_.dim[0], grid_.dim[1] + 1, grid_.dim[2], 1, dt);
    advect_component(grid_.w, grid_.adv_w, grid_.dim[0], grid_.dim[1], grid_.dim[2] + 1, 2, dt);
}

// 单个 MAC 分量的自平流（MacCormack 二阶修正 + 回溯点邻域限幅）。
//
// 不能用一阶半拉格朗日：回溯点上的三线性插值天然是低通的，遇到速度峰值
// （烟柱中心）时采样点必然落在更低的上游值上，每步削掉几个百分点。浮力
// 每步给的那点增量刚好被这份数值耗散吃掉，速度场就卡在一个很低的平衡值上
// —— 实测烟柱中心只到 0.17 m/s，而关掉自平流能到 1.1 m/s，烟根本升不起来。
// MacCormack 用"前向平流 → 反向平流回来 → 用两者的差修正"把一阶误差抵掉，
// 只在回溯点邻域内限幅防止过冲，峰值能保住。
void FluidDomain3D::advect_component(std::vector<float>& dst, const std::vector<float>& src,
                                     int sx, int sy, int sz, int axis, float dt) {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float scale = dt / grid_.cell;
    // 面心在自己的轴上是整数位置，另两轴在半格：样本坐标 = 面心坐标 - soff
    const float ox = (axis == 0) ? 0.0f : 0.5f;
    const float oy = (axis == 1) ? 0.0f : 0.5f;
    const float oz = (axis == 2) ? 0.0f : 0.5f;

    const auto snapshot_at = [&](float gx, float gy, float gz) {
        const float u = tri_sample(grid_.adv_u, nx + 1, ny, nz, gx, gy - 0.5f, gz - 0.5f);
        const float v = tri_sample(grid_.adv_v, nx, ny + 1, nz, gx - 0.5f, gy, gz - 0.5f);
        const float w = tri_sample(grid_.adv_w, nx, ny, nz + 1, gx - 0.5f, gy - 0.5f, gz);
        return math::Vector3f(u, v, w);
    };
    // 面两侧格子里较小（更"空气"）的 frac 决定自平流权重
    const auto face_weight = [&](int x, int y, int z) {
        int ax = x;
        int ay = y;
        int az = z;
        int bx = x;
        int by = y;
        int bz = z;
        if (axis == 0) { ax = std::clamp(x - 1, 0, nx - 1); bx = std::clamp(x, 0, nx - 1); }
        if (axis == 1) { ay = std::clamp(y - 1, 0, ny - 1); by = std::clamp(y, 0, ny - 1); }
        if (axis == 2) { az = std::clamp(z - 1, 0, nz - 1); bz = std::clamp(z, 0, nz - 1); }
        const float f0 = grid_.frac[idx_c(ax, ay, az)];
        const float f1 = grid_.frac[idx_c(bx, by, bz)];
        return std::clamp(1.0f - std::min(f0, f1) / k_air_frac, 0.0f, 1.0f);
    };

    // 前向平流：phi_hat = phi(x - u dt)。速度同时存进 mc_vel_ 供反向 pass 复用：
    // 反向 pass 用的是同一个面、同一份快照，没必要再三次三线性采样一遍。
    mc_scratch_.assign(src.size(), 0.0f);
    mc_vel_.assign(src.size() * 3, 0.0f);
    const auto forward_pass = [&](int x, int y, int z, size_t i) {
        const float gx = static_cast<float>(x) + ox;
        const float gy = static_cast<float>(y) + oy;
        const float gz = static_cast<float>(z) + oz;
        const math::Vector3f vel = snapshot_at(gx, gy, gz);
        mc_vel_[i * 3 + 0] = vel.x;
        mc_vel_[i * 3 + 1] = vel.y;
        mc_vel_[i * 3 + 2] = vel.z;
        mc_scratch_[i] = tri_sample(src, sx, sy, sz, gx - vel.x * scale - ox,
                                    gy - vel.y * scale - oy, gz - vel.z * scale - oz);
    };
    const auto clamp_bounds = [&](float bx, float by, float bz, float& lo, float& hi) {
        const int x0 = std::max(0, std::min(static_cast<int>(bx), sx - 2));
        const int y0 = std::max(0, std::min(static_cast<int>(by), sy - 2));
        const int z0 = std::max(0, std::min(static_cast<int>(bz), sz - 2));
        lo = src[0];
        hi = src[0];
        bool init = false;
        for (int dk = 0; dk < 2; ++dk) {
            for (int dj = 0; dj < 2; ++dj) {
                for (int di = 0; di < 2; ++di) {
                    const int xi = std::clamp(x0 + di, 0, sx - 1);
                    const int yi = std::clamp(y0 + dj, 0, sy - 1);
                    const int zi = std::clamp(z0 + dk, 0, sz - 1);
                    const float value = src[static_cast<size_t>((zi * sy + yi) * sx + xi)];
                    if (!init) { lo = value; hi = value; init = true; continue; }
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
            }
        }
    };

    const auto back_and_correct = [&](int x, int y, int z, size_t i) {
        const float weight = face_weight(x, y, z);
        if (weight <= 0.0f) return;
        const float gx = static_cast<float>(x) + ox;
        const float gy = static_cast<float>(y) + oy;
        const float gz = static_cast<float>(z) + oz;
        const math::Vector3f vel(mc_vel_[i * 3 + 0], mc_vel_[i * 3 + 1], mc_vel_[i * 3 + 2]);
        const float bx = gx - vel.x * scale - ox;
        const float by = gy - vel.y * scale - oy;
        const float bz = gz - vel.z * scale - oz;
        // 反向：把前向结果按同一速度搬回来（采样点 = 本面位置 + 一倍位移），
        // 与原始场之差即一阶插值误差。
        const float back =
            tri_sample(mc_scratch_, sx, sy, sz, bx + 2.0f * vel.x * scale,
                       by + 2.0f * vel.y * scale, bz + 2.0f * vel.z * scale);
        float res = mc_scratch_[i] + 0.5f * (src[i] - back);
        float lo = 0.0f;
        float hi = 0.0f;
        clamp_bounds(bx, by, bz, lo, hi);
        res = std::clamp(res, std::min(lo, src[i]), std::max(hi, src[i]));
        float& target = dst[i];
        target += (res - target) * weight;
    };

    // 三个分量的数组在各自的尺寸下布局一致（idx = (z*sy + y)*sx + x），
    // 所以遍历与下标可以共用，只有面心位置与写入目标按轴区分。
    const auto run_pass = [&](bool forward) {
        for (int z = 0; z < sz; ++z) {
            for (int y = 0; y < sy; ++y) {
                for (int x = 0; x < sx; ++x) {
                    const size_t i = static_cast<size_t>((z * sy + y) * sx + x);
                    if (forward) {
                        forward_pass(x, y, z, i);
                    } else {
                        back_and_correct(x, y, z, i);
                    }
                }
            }
        }
    };
    run_pass(true);
    run_pass(false);
}

// 密度场半拉格朗日平流（RK2 回溯）。先用中点位置回溯一次，再在最终位置采样。
void FluidDomain3D::advect_density(float dt) {
    if (!gas_enabled) return;
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    grid_.tmp.assign(grid_.density.begin(), grid_.density.end());

    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const float gx = static_cast<float>(x) + 0.5f;
                const float gy = static_cast<float>(y) + 0.5f;
                const float gz = static_cast<float>(z) + 0.5f;
                // 对流的驱动速度 = 网格速度 + 浮力项。浮力项必须显式加在
                // 平流速度上，而不是只写进网格速度场：投影会把网格里的竖直
                // 分量当成要消掉的散度压回 0，烟就只能堆在源口不动。
                const math::Vector3f v0 = sample_cell_velocity(gx, gy, gz);
                const float mxd = gx - v0.x * (dt / grid_.cell) * 0.5f;
                const float myd = gy - v0.y * (dt / grid_.cell) * 0.5f;
                const float mzd = gz - v0.z * (dt / grid_.cell) * 0.5f;
                const float d_mid = sample_cell_scalar(grid_.tmp, mxd, myd, mzd);
                const math::Vector3f v1(v0.x,
                                        v0.y + gas_buoyancy * d_mid * k_buoyancy_advect,
                                        v0.z);
                const float bx = gx - v1.x * (dt / grid_.cell);
                const float by = gy - v1.y * (dt / grid_.cell);
                const float bz = gz - v1.z * (dt / grid_.cell);
                grid_.density[idx_c(x, y, z)] = sample_cell_scalar(grid_.tmp, bx, by, bz);
            }
        }
    }

    // 液相格里不该有烟。水是半透明的，留在池里的烟雾等值面会透过水面显出来，
    // 表现为水面以下冒出一个方块状的实心团。按液体分数直接清掉，水线就成了
    // 烟的下边界。
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                if (grid_.frac[c] >= k_air_frac) grid_.density[c] = 0.0f;
            }
        }
    }
}

void FluidDomain3D::dissipate_gas(float dt) {
    if (!gas_enabled) return;
    const float keep = std::max(0.0f, 1.0f - gas_dissipation * dt);
    for (float& d : grid_.density) d *= keep;
}

// 液体格判定用的 frac 平滑。投影把 frac < 0.5 的格当空气（p = 0），
// 单格 frac 一旦抖动就会在液体内部撕出一个"空气洞"，压力随之泄漏、整片水
// 塌下去。做一次 7 点平均把这种孤立噪点抹掉。
void FluidDomain3D::smooth_frac() {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(idx_c(x, y, z));
                float sum = grid_.frac[c] * 2.0f;
                float wsum = 2.0f;
                const auto add = [&](int xx, int yy, int zz) {
                    if (xx < 0 || xx >= nx || yy < 0 || yy >= ny || zz < 0 || zz >= nz) return;
                    sum += grid_.frac[idx_c(xx, yy, zz)];
                    wsum += 1.0f;
                };
                add(x - 1, y, z);
                add(x + 1, y, z);
                add(x, y - 1, z);
                add(x, y + 1, z);
                add(x, y, z - 1);
                add(x, y, z + 1);
                grid_.p_next[c] = sum / wsum;
            }
        }
    }
    grid_.frac.swap(grid_.p_next);
}

// 压力投影：解 ∇²p = div(u*)/dt，再把 u 减去 ∇p*dt。
// 空气格（frac < 0.5）用 Dirichlet p = 0，域边界用 Neumann（镜像自身）。
//
// 求解器是红黑 Gauss-Seidel。Jacobi 的收敛半径是 1-O(1/N²)，44³ 的域要几千次
// 才收敛；只跑几十次 Jacobi 会留下大量残余散度，表现为水被压塌、体积越算越小。
// GS 就地更新、收敛快一倍多，再靠 warm start（沿用上一步的压力当初值）省掉大头。
void FluidDomain3D::project(float dt) {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const float h = grid_.cell;
    const float inv_h = 1.0f / h;

    smooth_frac();

    // 固壁的法向速度必须在算散度**之前**置零。放在减梯度阶段才清零的话，
    // 算散度时底部面心还留着向下的速度，进出口通量相互抵消得到散度 0，
    // 压力永远是 0 —— 水柱得不到静压支撑，整体自由落体，最后被压成一薄层。
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            grid_.u[idx_u(0, y, z)] = 0.0f;
            grid_.u[idx_u(nx, y, z)] = 0.0f;
        }
    }
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            grid_.v[idx_v(x, 0, z)] = 0.0f;
            grid_.v[idx_v(x, ny, z)] = 0.0f;
        }
    }
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            grid_.w[idx_w(x, y, 0)] = 0.0f;
            grid_.w[idx_w(x, y, nz)] = 0.0f;
        }
    }

    // FLIP 差量用的投影前速度
    grid_.prev_u = grid_.u;
    grid_.prev_v = grid_.v;
    grid_.prev_w = grid_.w;

    // 散度只统计"至少一侧是液体"的面。液体与空气的交界面在 P2G 之后速度
    // 是单侧外推值，直接算进散度会让边界格得到巨大散度，压力梯度随之变大，
    // 水被从表面炸开（表现为整片水碎成细丝洒满整个域）。
    const auto liquid_cell = [&](int x, int y, int z) {
        if (x < 0 || x >= nx || y < 0 || y >= ny || z < 0 || z >= nz) return false;
        return grid_.frac[idx_c(x, y, z)] >= k_air_frac;
    };
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                float du = 0.0f;
                float dv = 0.0f;
                float dw = 0.0f;
                if (liquid_cell(x - 1, y, z) || liquid_cell(x, y, z)) {
                    du = grid_.u[idx_u(x + 1, y, z)] - grid_.u[idx_u(x, y, z)];
                }
                if (liquid_cell(x, y - 1, z) || liquid_cell(x, y, z)) {
                    dv = grid_.v[idx_v(x, y + 1, z)] - grid_.v[idx_v(x, y, z)];
                }
                if (liquid_cell(x, y, z - 1) || liquid_cell(x, y, z)) {
                    dw = grid_.w[idx_w(x, y, z + 1)] - grid_.w[idx_w(x, y, z)];
                }
                grid_.div[idx_c(x, y, z)] = (du + dv + dw) * inv_h;
            }
        }
    }

    const auto is_air = [&](int x, int y, int z) {
        return grid_.frac[idx_c(x, y, z)] < k_air_frac;
    };

    const int iterations = std::clamp(pressure_iterations, 4, 200);
    for (int it = 0; it < iterations; ++it) {
        for (int color = 0; color < 2; ++color) {
            for (int z = 0; z < nz; ++z) {
                for (int y = 0; y < ny; ++y) {
                    const int parity = ((y + z) & 1) ^ color;
                    for (int x = parity; x < nx; x += 2) {
                        const size_t c = static_cast<size_t>(idx_c(x, y, z));
                        if (is_air(x, y, z)) {
                            grid_.p[c] = 0.0f;
                            continue;
                        }
                        float sum = 0.0f;
                        int count = 0;
                        const auto nb = [&](int xx, int yy, int zz) {
                            if (xx < 0 || xx >= nx || yy < 0 || yy >= ny || zz < 0 || zz >= nz) {
                                // 固体壁：Neumann，镜像自身压力
                                sum += grid_.p[c];
                            } else if (is_air(xx, yy, zz)) {
                                sum += 0.0f;  // 自由表面：p = 0
                            } else {
                                sum += grid_.p[idx_c(xx, yy, zz)];
                            }
                            ++count;
                        };
                        nb(x - 1, y, z);
                        nb(x + 1, y, z);
                        nb(x, y - 1, z);
                        nb(x, y + 1, z);
                        nb(x, y, z - 1);
                        nb(x, y, z + 1);
                        const float target = grid_.div[c] * h * h / dt;
                        grid_.p[c] = (sum - target) / static_cast<float>(std::max(1, count));
                    }
                }
            }
        }
    }

    // 减梯度。面心两侧若有一侧是空气，那里压力按 0 算（自由表面），
    // 所以水面外的速度被完整释放，不会把水面吸住。
    const auto pressure_at = [&](int x, int y, int z) {
        if (x < 0 || x >= nx || y < 0 || y >= ny || z < 0 || z >= nz) return 0.0f;
        const size_t c = static_cast<size_t>(idx_c(x, y, z));
        return grid_.frac[c] < k_air_frac ? 0.0f : grid_.p[c];
    };

    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x <= nx; ++x) {
                float& u = grid_.u[idx_u(x, y, z)];
                if (x == 0 || x == nx) {
                    u = 0.0f;
                    continue;
                }
                u -= dt * (pressure_at(x, y, z) - pressure_at(x - 1, y, z)) * inv_h;
            }
        }
    }
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y <= ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                float& v = grid_.v[idx_v(x, y, z)];
                if (y == 0 || y == ny) {
                    v = 0.0f;
                    continue;
                }
                v -= dt * (pressure_at(x, y, z) - pressure_at(x, y - 1, z)) * inv_h;
            }
        }
    }
    for (int z = 0; z <= nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                float& w = grid_.w[idx_w(x, y, z)];
                if (z == 0 || z == nz) {
                    w = 0.0f;
                    continue;
                }
                w -= dt * (pressure_at(x, y, z) - pressure_at(x, y, z - 1)) * inv_h;
            }
        }
    }
}

// G2P：把投影前后的速度差按 FLIP 混合回粒子。
//   v_new = flip * (v_old + (u_new - u_old)_interp) + (1 - flip) * u_new_interp
// flip=1 完全保留粒子自身速度（有飞溅、有涡），flip=0 退化成纯 PIC（稳但粘）。
void FluidDomain3D::grid_to_particles() {
    const float h = grid_.cell;
    const float flip = std::clamp(flip_ratio, 0.0f, 1.0f);
    for (size_t i = 0; i < positions_.size(); ++i) {
        const math::Vector3f& pos = positions_[i];
        const float gx = pos.x / h - grid_.origin.x / h;
        const float gy = pos.y / h - grid_.origin.y / h;
        const float gz = pos.z / h - grid_.origin.z / h;
        const math::Vector3f now = sample_cell_velocity(gx, gy, gz);

        const int nx = grid_.dim[0];
        const int ny = grid_.dim[1];
        const int nz = grid_.dim[2];
        const float pux = tri_sample(grid_.prev_u, nx + 1, ny, nz, gx, gy - 0.5f, gz - 0.5f);
        const float puy = tri_sample(grid_.prev_v, nx, ny + 1, nz, gx - 0.5f, gy, gz - 0.5f);
        const float puz = tri_sample(grid_.prev_w, nx, ny, nz + 1, gx - 0.5f, gy - 0.5f, gz);
        const math::Vector3f delta(now.x - pux, now.y - puy, now.z - puz);

        const math::Vector3f& v = velocities_[i];
        velocities_[i] = math::Vector3f(v.x + delta.x * flip + now.x * (1.0f - flip),
                                        v.y + delta.y * flip + now.y * (1.0f - flip),
                                        v.z + delta.z * flip + now.z * (1.0f - flip));
    }
}

// 粒子邻域：计数排序空间哈希。格边长取播种间距，查询半径 1 格。
// 与 SPHFluid3D 同一套做法：前缀和 + 两段连续数组，27 邻域遍历只读连续内存。
void FluidDomain3D::build_particle_grid() {
    pgrid_cell_ = std::max(1e-3f, spacing_);
    // 网格范围按域的边长算，粒子的活动范围不会超出域
    pgrid_origin_ = math::Vector3f(-domain_extents.x, -domain_extents.y, -domain_extents.z) -
                    math::Vector3f(pgrid_cell_, pgrid_cell_, pgrid_cell_);
    for (int a = 0; a < 3; ++a) {
        const float extent = (&domain_extents.x)[a] * 2.0f + pgrid_cell_ * 3.0f;
        pgrid_dim_[a] = std::max(1, static_cast<int>(extent / pgrid_cell_) + 1);
    }
    const int nx = pgrid_dim_[0];
    const int ny = pgrid_dim_[1];
    const int nz = pgrid_dim_[2];
    const size_t cells = static_cast<size_t>(nx) * ny * nz;
    if (cell_start_.size() != cells + 1) cell_start_.assign(cells + 1, 0);
    else std::fill(cell_start_.begin(), cell_start_.end(), 0);
    cell_of_.assign(positions_.size(), 0);

    const auto cell_index = [&](const math::Vector3f& p) {
        int cx = static_cast<int>((p.x - pgrid_origin_.x) / pgrid_cell_);
        int cy = static_cast<int>((p.y - pgrid_origin_.y) / pgrid_cell_);
        int cz = static_cast<int>((p.z - pgrid_origin_.z) / pgrid_cell_);
        cx = std::clamp(cx, 0, nx - 1);
        cy = std::clamp(cy, 0, ny - 1);
        cz = std::clamp(cz, 0, nz - 1);
        return (cz * ny + cy) * nx + cx;
    };

    for (size_t i = 0; i < positions_.size(); ++i) {
        const int c = cell_index(positions_[i]);
        cell_of_[i] = c;
        ++cell_start_[static_cast<size_t>(c) + 1];
    }
    for (size_t i = 0; i < cells; ++i) cell_start_[i + 1] += cell_start_[i];
    sorted_.assign(positions_.size(), 0);
    cell_cursor_.assign(cell_start_.begin(), cell_start_.end() - 1);
    for (size_t i = 0; i < positions_.size(); ++i) {
        sorted_[static_cast<size_t>(cell_cursor_[static_cast<size_t>(cell_of_[i])]++)] =
            static_cast<int>(i);
    }
}

// 粒子最小距离约束（PBD 式）。
//
// 纯 FLIP 的网格压力只改**速度**，不改位置。粒子一旦被挤到间距小于静止间距，
// 速度场又是均匀的（散度 0、压力 0），就再没有任何机制把它们分开 —— 水会
// 永久压扁成薄薄一层，看起来像"水塌了"。这里直接对粒子对做最小距离约束
// （欠松弛 0.5，一次迭代即可），把体积恢复回来。
void FluidDomain3D::solve_particle_spacing() {
    if (positions_.size() < 2) return;
    build_particle_grid();
    const int nx = pgrid_dim_[0];
    const int ny = pgrid_dim_[1];
    const int nz = pgrid_dim_[2];
    const float target = spacing_;
    const float inv_cell = 1.0f / pgrid_cell_;
    constexpr float k_relax = 0.5f;

    for (size_t i = 0; i < positions_.size(); ++i) {
        math::Vector3f& pi = positions_[i];
        int cx = static_cast<int>((pi.x - pgrid_origin_.x) * inv_cell);
        int cy = static_cast<int>((pi.y - pgrid_origin_.y) * inv_cell);
        int cz = static_cast<int>((pi.z - pgrid_origin_.z) * inv_cell);
        cx = std::clamp(cx, 0, nx - 1);
        cy = std::clamp(cy, 0, ny - 1);
        cz = std::clamp(cz, 0, nz - 1);
        for (int dz = -1; dz <= 1; ++dz) {
            const int zz = cz + dz;
            if (zz < 0 || zz >= nz) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = cy + dy;
                if (yy < 0 || yy >= ny) continue;
                const int row = (zz * ny + yy) * nx;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = cx + dx;
                    if (xx < 0 || xx >= nx) continue;
                    const size_t c = static_cast<size_t>(row + xx);
                    const int begin = cell_start_[c];
                    const int end = cell_start_[c + 1];
                    for (int k = begin; k < end; ++k) {
                        const int j = sorted_[static_cast<size_t>(k)];
                        if (j <= static_cast<int>(i)) continue;
                        math::Vector3f& pj = positions_[static_cast<size_t>(j)];
                        const math::Vector3f d = pj - pi;
                        const float dist = d.length();
                        if (dist >= target || dist < 1e-7f) continue;
                        const float push = (target - dist) * k_relax * 0.5f;
                        const math::Vector3f n = d * (1.0f / dist);
                        pi = pi - n * push;
                        pj = pj + n * push;
                    }
                }
            }
        }
    }
}

// 液体撞域壁：位置夹回、法向速度反弹并衰减。
void FluidDomain3D::collide_domain() {
    const float damping = 0.25f;
    const math::Vector3f lim(domain_extents.x - grid_.cell * 0.5f,
                             domain_extents.y - grid_.cell * 0.5f,
                             domain_extents.z - grid_.cell * 0.5f);
    for (size_t i = 0; i < positions_.size(); ++i) {
        math::Vector3f& p = positions_[i];
        math::Vector3f& v = velocities_[i];
        if (p.x < -lim.x) { p.x = -lim.x; if (v.x < 0.0f) v.x *= -damping; }
        if (p.x > lim.x) { p.x = lim.x; if (v.x > 0.0f) v.x *= -damping; }
        if (p.y < -lim.y) { p.y = -lim.y; if (v.y < 0.0f) v.y *= -damping; }
        if (p.y > lim.y) { p.y = lim.y; if (v.y > 0.0f) v.y *= -damping; }
        if (p.z < -lim.z) { p.z = -lim.z; if (v.z < 0.0f) v.z *= -damping; }
        if (p.z > lim.z) { p.z = lim.z; if (v.z > 0.0f) v.z *= -damping; }
    }
}

void FluidDomain3D::simulate_step(float dt) {
    ++step_index_;
    if (liquid_enabled) {
        integrate_particles(dt);
        particles_to_grid();
    } else {
        std::fill(grid_.frac.begin(), grid_.frac.end(), 0.0f);
        std::fill(grid_.u.begin(), grid_.u.end(), 0.0f);
        std::fill(grid_.v.begin(), grid_.v.end(), 0.0f);
        std::fill(grid_.w.begin(), grid_.w.end(), 0.0f);
    }
    emit_gas(dt);
    apply_wind(dt);
    // 力先加进速度场，再由速度场自己把动量搬运一遍（u·∇u），最后才做涡量
    // 约束：先搬运再放大，涡量才能被卷到下游；反过来放大完立刻被平流抹掉。
    apply_buoyancy(dt);
    advect_velocity(dt);
    apply_vorticity(dt);
    advect_density(dt);
    project(dt);
    if (liquid_enabled) {
        grid_to_particles();
        // 位置级不可压：网格压力只管速度，粒子被挤到一起后需要它把体积撑回来
        solve_particle_spacing();
        // CFL 保护：一帧内位移超过半格就会穿过压力场，投影失效后速度自我
        // 放大成爆炸。这里按半格/步硬夹一次，数值异常时只会停顿而不会发散。
        const float vmax = 0.5f * grid_.cell / std::max(1e-4f, dt);
        for (auto& v : velocities_) {
            const float len = v.length();
            if (len > vmax) v = v * (vmax / len);
        }
        collide_domain();
    }
    dissipate_gas(dt);
}

void FluidDomain3D::splash(const math::Vector3f& local_center, float radius, float strength) {
    const float r2 = radius * radius;
    for (size_t i = 0; i < positions_.size(); ++i) {
        const math::Vector3f d = positions_[i] - local_center;
        const float d2 = d.length_sq();
        if (d2 > r2 || d2 < 1e-8f) continue;
        const float falloff = 1.0f - d2 / r2;
        velocities_[i] = velocities_[i] + d.normalized() * (strength * falloff);
    }
}

// ---------------------------------------------------------------------------
// 表面重建
// ---------------------------------------------------------------------------

// 液体稠密场：粒子按三线性权重摊到**角点**，再除以每格期望粒子数，得到 [0,1]
// 的液体占据程度。
//
// 场必须是角点布局（每轴 nx+1 个样本）：surface_nets::extract 按角点索引读取，
// 直接塞一份格心场（每轴 nx 个）会让它按错误的步长遍历，输出整片沿 y 方向
// 错位的水平条带。
void FluidDomain3D::build_liquid_field() {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const int cx = nx + 1;
    const int cy = ny + 1;
    liquid_field_.assign(static_cast<size_t>(cx) * cy * (nz + 1), 0.0f);
    const float h = grid_.cell;

    for (const auto& pos : positions_) {
        const float gx = (pos.x - grid_.origin.x) / h;
        const float gy = (pos.y - grid_.origin.y) / h;
        const float gz = (pos.z - grid_.origin.z) / h;
        const int x0 = static_cast<int>(std::floor(gx));
        const int y0 = static_cast<int>(std::floor(gy));
        const int z0 = static_cast<int>(std::floor(gz));
        const float fx = gx - static_cast<float>(x0);
        const float fy = gy - static_cast<float>(y0);
        const float fz = gz - static_cast<float>(z0);
        for (int dk = 0; dk < 2; ++dk) {
            const int zi = z0 + dk;
            if (zi < 0 || zi > nz) continue;
            for (int dj = 0; dj < 2; ++dj) {
                const int yi = y0 + dj;
                if (yi < 0 || yi > ny) continue;
                for (int di = 0; di < 2; ++di) {
                    const int xi = x0 + di;
                    if (xi < 0 || xi > nx) continue;
                    const float weight = (di ? fx : 1.0f - fx) * (dj ? fy : 1.0f - fy) *
                                         (dk ? fz : 1.0f - fz);
                    if (weight <= 0.0f) continue;
                    liquid_field_[static_cast<size_t>((zi * cy + yi) * cx + xi)] += weight;
                }
            }
        }
    }
    const float per_cell =
        std::max(0.5f, std::pow(grid_.cell / std::max(1e-4f, spacing_), 3.0f));
    const float inv = 1.0f / per_cell;
    for (float& v : liquid_field_) v = std::clamp(v * inv, 0.0f, 1.0f);

    field_dim_[0] = nx;
    field_dim_[1] = ny;
    field_dim_[2] = nz;
    field_origin_ = grid_.origin;
    field_cell_ = h;
}

// 烟雾密度本来存在格心，这里插值到角点，与液体场共用同一套提取代码。
void FluidDomain3D::build_smoke_field() {
    const int nx = grid_.dim[0];
    const int ny = grid_.dim[1];
    const int nz = grid_.dim[2];
    const int cx = nx + 1;
    const int cy = ny + 1;
    smoke_field_.assign(static_cast<size_t>(cx) * cy * (nz + 1), 0.0f);
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                smoke_field_[static_cast<size_t>((k * cy + j) * cx + i)] =
                    sample_cell_scalar(grid_.density, static_cast<float>(i),
                                       static_cast<float>(j), static_cast<float>(k));
            }
        }
    }

    // 一次 7 点平滑再提取。烟柱侧面在网格上是一级一级的台阶，直接取等值面会
    // 沿台阶拉出一排竖直细长面片（侧视角看就是"竖条纹"），法线在面片之间还
    // 会跳变。先抹一遍，等值面就成了一张连续曲面。
    smoke_scratch_.assign(smoke_field_.size(), 0.0f);
    const auto at = [&](int i, int j, int k) {
        const int ii = std::clamp(i, 0, cx - 1);
        const int jj = std::clamp(j, 0, cy - 1);
        const int kk = std::clamp(k, 0, nz);
        return smoke_field_[static_cast<size_t>((kk * cy + jj) * cx + ii)];
    };
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                const size_t c = static_cast<size_t>((k * cy + j) * cx + i);
                smoke_scratch_[c] = (at(i, j, k) * 2.0f + at(i - 1, j, k) + at(i + 1, j, k) +
                                     at(i, j - 1, k) + at(i, j + 1, k) + at(i, j, k - 1) +
                                     at(i, j, k + 1)) /
                                    8.0f;
            }
        }
    }
    smoke_field_.swap(smoke_scratch_);

    field_dim_[0] = nx;
    field_dim_[1] = ny;
    field_dim_[2] = nz;
    field_origin_ = grid_.origin;
    field_cell_ = grid_.cell;
}

void FluidDomain3D::rebuild_surface() {
    // 层 0 = 液体表面
    if (liquid_enabled && !layers_.empty()) {
        build_liquid_field();
        auto mesh = std::make_shared<assets::MeshData>();
        surface_nets::extract(liquid_field_.data(), field_dim_[0], field_dim_[1], field_dim_[2],
                              field_origin_, field_cell_, liquid_iso, *mesh);
        mesh->name = "FluidDomainLiquid";
        Layer& layer = *layers_[0];
        if (!mesh->empty()) {
            layer.geometry = std::move(mesh);
            ++layer.generation;
            layer.dirty = true;
        }
    }

    // 其后各层 = 烟雾的不同密度等值面。iso 由内向外递减，形成相互包裹的
    // 几层半透明壳，叠加起来像有厚度的体积。
    if (gas_enabled && layers_.size() > static_cast<size_t>(liquid_enabled ? 1 : 0)) {
        build_smoke_field();
        const int shells = static_cast<int>(layers_.size()) - (liquid_enabled ? 1 : 0);
        for (int s = 0; s < shells; ++s) {
            // 由内向外逐层降低阈值：内层贴着高密度核心，外层包住稀薄的边缘。
            const float iso = std::max(0.01f, smoke_iso * std::pow(0.55f, static_cast<float>(s)));
            auto mesh = std::make_shared<assets::MeshData>();
            surface_nets::extract(smoke_field_.data(), field_dim_[0], field_dim_[1], field_dim_[2],
                                  field_origin_, field_cell_, iso, *mesh);
            mesh->name = "FluidDomainSmoke";
            Layer& layer = *layers_[static_cast<size_t>((liquid_enabled ? 1 : 0) + s)];
            if (mesh->empty()) continue;
            layer.geometry = std::move(mesh);
            ++layer.generation;
            layer.dirty = true;
        }
    }
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------

void FluidDomain3D::invalidate_gpu() {
    if (ctx_) {
        for (auto& layer : layers_) {
            if (!layer) continue;
            if (layer->mesh.is_valid()) ctx_->destroy_mesh(layer->mesh);
            if (layer->material) layer->material->destroy_gpu(ctx_);
            layer->mesh = render::RHIMeshHandle{};
            layer->queued.store(0, std::memory_order_release);
            layer->uploaded.store(false, std::memory_order_release);
        }
    }
    layers_.clear();
    ctx_ = nullptr;
}

render::RHIMeshHandle FluidDomain3D::layer_mesh(int index) const {
    if (index < 0 || index >= static_cast<int>(layers_.size())) return render::RHIMeshHandle{};
    return layers_[static_cast<size_t>(index)]->mesh;
}

const render::Material* FluidDomain3D::layer_material(int index) const {
    if (index < 0 || index >= static_cast<int>(layers_.size())) return nullptr;
    return layers_[static_cast<size_t>(index)]->material.get();
}

void FluidDomain3D::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                                 float& radius) const {
    center = world.transform_point(math::Vector3f::zero());
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = domain_extents.length() * std::max(sx, std::max(sy, sz));
}

void FluidDomain3D::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    if (ctx_ != ctx) {
        if (ctx_) invalidate_gpu();
        ctx_ = ctx;
    }

    // 层数与开关同步：液体 1 层 + 烟雾 smoke_shells 层
    const size_t want = (liquid_enabled ? 1u : 0u) +
                        (gas_enabled ? static_cast<size_t>(std::clamp(smoke_shells, 1, 4)) : 0u);
    if (layers_.size() != want) {
        for (auto& layer : layers_) {
            if (!layer) continue;
            if (layer->mesh.is_valid()) ctx_->destroy_mesh(layer->mesh);
            if (layer->material) layer->material->destroy_gpu(ctx_);
        }
        layers_.clear();
        layers_.reserve(want);
        for (size_t i = 0; i < want; ++i) {
            auto layer = std::make_unique<Layer>();
            layer->material = std::make_unique<render::Material>();
            layers_.push_back(std::move(layer));
        }
        smoke_layer_first_ = liquid_enabled ? 1u : 0u;
    }

    // 材质参数每帧刷新（颜色可在运行时改）
    for (size_t i = 0; i < layers_.size(); ++i) {
        render::Material* mat = layers_[i]->material.get();
        if (!mat) continue;
        mat->blend_mode = render::Material::BlendMode::Blend;
        mat->two_sided = true;
        if (liquid_enabled && i == 0) {
            mat->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
            mat->opacity = water_color.w;
            mat->roughness = water_roughness;
            mat->metallic = water_metallic;
            mat->emissive_color = math::Vector3f::zero();
        } else {
            // 烟雾：越靠外的壳越淡，靠内的壳更实。
            // 极弱的自发光只用来让背光面不至于纯黑；给多了整束烟会像一根
            // 发光的白柱子，失去半透明的体积感。
            const int shell = static_cast<int>(i) - static_cast<int>(smoke_layer_first_);
            const float scale = 1.0f + 0.55f * static_cast<float>(std::max(0, shell));
            mat->albedo_color = math::Vector3f(smoke_color.x, smoke_color.y, smoke_color.z);
            mat->opacity = std::clamp(smoke_color.w * scale, 0.02f, 0.85f);
            mat->roughness = 1.0f;
            mat->metallic = 0.0f;
            mat->emissive_color =
                math::Vector3f(smoke_color.x, smoke_color.y, smoke_color.z) * 0.03f;
        }
    }

    ++frame_counter_;
    const int interval = std::clamp(surface_update_interval, 1, 8);
    if (frame_counter_ % interval == 0) rebuild_surface();

    for (size_t i = 0; i < layers_.size(); ++i) {
        Layer& layer = *layers_[i];
        if (!layer.geometry || layer.geometry->empty()) continue;

        const uint64_t generation = layer.generation;
        const bool need_geometry = generation != layer.queued.load(std::memory_order_acquire);
        const bool need_material = layer.material && !layer.uploaded.load(std::memory_order_acquire);
        if (!need_geometry && !need_material) continue;

        auto geometry = layer.geometry;
        if (need_geometry) layer.queued.store(generation, std::memory_order_release);

        Layer* layer_ptr = &layer;
        ctx->push_command([this, ctx, geometry, need_geometry, layer_ptr,
                           token = alive_token_](render::IRenderBackend*) {
            if (!token->load(std::memory_order_acquire)) return;

            if (need_geometry) {
                if (!layer_ptr->mesh.is_valid()) {
                    layer_ptr->mesh = ctx->create_mesh();
                    render::IMesh* mesh = ctx->mesh(layer_ptr->mesh);
                    if (!mesh) {
                        layer_ptr->mesh = render::RHIMeshHandle{};
                        return;
                    }
                    render::VertexLayout layout;
                    layout.stride = sizeof(assets::MeshVertex);
                    layout.attributes = {
                        {0, render::VertexType::Float3, false, 0},
                        {1, render::VertexType::Float3, false, 12},
                        {2, render::VertexType::Float3, false, 24},
                        {3, render::VertexType::Float2, false, 36},
                        {4, render::VertexType::Float3, false, 44}
                    };
                    mesh->set_layout(layout);
                }
                render::IMesh* mesh = ctx->mesh(layer_ptr->mesh);
                if (mesh) {
                    const uint32_t vcount = static_cast<uint32_t>(geometry->vertices.size());
                    const uint32_t icount = static_cast<uint32_t>(geometry->indices.size());
                    mesh->upload_vertices(geometry->vertices.data(),
                                          static_cast<uint32_t>(vcount * sizeof(assets::MeshVertex)),
                                          vcount);
                    mesh->upload_indices(geometry->indices.data(),
                                         static_cast<uint32_t>(icount * sizeof(uint32_t)), icount);
                }
            }
            if (layer_ptr->material &&
                !layer_ptr->uploaded.exchange(true, std::memory_order_acq_rel)) {
                layer_ptr->material->upload_to_gpu(ctx);
            }
        });
    }
}

} // namespace gryce_engine::components
