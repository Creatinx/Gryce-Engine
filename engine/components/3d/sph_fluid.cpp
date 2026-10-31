#include "components/3d/sph_fluid.h"

#include <algorithm>
#include <cmath>

#include "render/material.h"
#include "render/mesh.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_fixed_dt = 1.0f / 60.0f;
constexpr int k_max_substeps = 2;
// 网格上限：三轴格数夹取范围（上限 64³ ≈ 26 万格，计数排序数组 ~3MB）
constexpr int k_grid_max_dim = 64;
constexpr int k_surface_min_res = 8;
constexpr int k_surface_max_res = 64;

// Müller 2003 核函数（3D）：
// poly6  W(r,h)   = 315/(64 π h^9) (h²-r²)³          （密度 / splat）
// spiky  ∇W(r,h)  = -45/(π h^6) (h-r)² * (r_vec/r)   （压力梯度）
struct SphKernel {
    float h = 0.12f;
    float h2 = 0.0f;
    float poly6 = 0.0f;
    float spiky = 0.0f;

    explicit SphKernel(float radius) : h(radius) {
        h2 = h * h;
        const float h6 = h2 * h2 * h2;
        const float h9 = h6 * h2 * h;
        poly6 = 315.0f / (64.0f * k_pi * h9);
        spiky = -45.0f / (k_pi * h6);
    }
};

inline float poly6_value(const SphKernel& k, float r2) {
    if (r2 >= k.h2) return 0.0f;
    const float d = k.h2 - r2;
    return k.poly6 * d * d * d;
}

// 单元 8 角点在 (x,y,z) 上的线性下标：c = x + 2y + 4z
inline int corner_index(int dx, int dy, int dz) { return dx + 2 * dy + 4 * dz; }

} // namespace

SPHFluid3D::SPHFluid3D() {
    material_ = std::make_unique<render::Material>();
    material_->blend_mode = render::Material::BlendMode::Blend;
    material_->two_sided = true;
    material_->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
    material_->opacity = water_color.w;
    material_->roughness = roughness;
    material_->metallic = metallic;
    reset_particles();
}

SPHFluid3D::~SPHFluid3D() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (material_) material_->destroy_gpu(ctx_);
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

void SPHFluid3D::on_destroy() {
    alive_token_->store(false, std::memory_order_release);
}

render::Material* SPHFluid3D::ensure_material() {
    if (!material_) material_ = std::make_unique<render::Material>();
    return material_.get();
}

void SPHFluid3D::serialize(nlohmann::json& out) const {
    out["spawn_extents"] = {spawn_extents.x, spawn_extents.y, spawn_extents.z};
    out["spacing"] = spacing;
    out["max_particles"] = max_particles;
    out["rest_density"] = rest_density;
    out["stiffness"] = stiffness;
    out["viscosity"] = viscosity;
    out["gravity"] = {gravity.x, gravity.y, gravity.z};
    out["buoyancy_strength"] = buoyancy_strength;
    out["linear_drag"] = linear_drag;
    out["flow_velocity"] = {flow_velocity.x, flow_velocity.y, flow_velocity.z};
    out["container_extents"] = {container_extents.x, container_extents.y, container_extents.z};
    out["wall_damping"] = wall_damping;
    out["surface_resolution"] = surface_resolution;
    out["surface_iso"] = surface_iso;
    out["surface_update_interval"] = surface_update_interval;
    out["water_color"] = {water_color.x, water_color.y, water_color.z, water_color.w};
    out["roughness"] = roughness;
    out["metallic"] = metallic;
    out["simulate"] = simulate;
}

void SPHFluid3D::deserialize(const nlohmann::json& in) {
    const auto read_vec3 = [&](const char* key, math::Vector3f& dst) {
        if (in.contains(key) && in[key].is_array() && in[key].size() == 3) {
            dst = math::Vector3f(in[key].at(0).get<float>(), in[key].at(1).get<float>(),
                                 in[key].at(2).get<float>());
        }
    };
    read_vec3("spawn_extents", spawn_extents);
    spacing = in.value("spacing", spacing);
    max_particles = in.value("max_particles", max_particles);
    rest_density = in.value("rest_density", rest_density);
    stiffness = in.value("stiffness", stiffness);
    viscosity = in.value("viscosity", viscosity);
    read_vec3("gravity", gravity);
    buoyancy_strength = in.value("buoyancy_strength", buoyancy_strength);
    linear_drag = in.value("linear_drag", linear_drag);
    read_vec3("flow_velocity", flow_velocity);
    read_vec3("container_extents", container_extents);
    wall_damping = in.value("wall_damping", wall_damping);
    surface_resolution = in.value("surface_resolution", surface_resolution);
    surface_iso = in.value("surface_iso", surface_iso);
    surface_update_interval = in.value("surface_update_interval", surface_update_interval);
    if (in.contains("water_color") && in["water_color"].is_array() &&
        in["water_color"].size() == 4) {
        const auto& c = in["water_color"];
        water_color = math::Vector4f(c.at(0).get<float>(), c.at(1).get<float>(),
                                     c.at(2).get<float>(), c.at(3).get<float>());
    }
    roughness = in.value("roughness", roughness);
    metallic = in.value("metallic", metallic);
    simulate = in.value("simulate", simulate);

    material_->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
    material_->opacity = water_color.w;
    material_->roughness = roughness;
    material_->metallic = metallic;
    reset_particles();
}

void SPHFluid3D::reset_particles() {
    positions_.clear();
    velocities_.clear();
    positions_prev_.clear();
    densities_.clear();
    lambdas_.clear();

    spacing = std::max(0.005f, spacing);
    const float dx = spacing;
    const int cap = std::max(1, max_particles);
    const int nx = std::max(1, static_cast<int>(spawn_extents.x * 2.0f / dx));
    const int ny = std::max(1, static_cast<int>(spawn_extents.y * 2.0f / dx));
    const int nz = std::max(1, static_cast<int>(spawn_extents.z * 2.0f / dx));

    // 出生块贴住容器底：悬空生成会让整块水自由落体撞底，SPH 在撞击瞬间
    // 出现远超静止压力的尖峰，水面被打散成飞沫（表面重建随之碎裂）。
    float base_y = -spawn_extents.y;
    if (container_extents.x > 0.0f && container_extents.y > 0.0f && container_extents.z > 0.0f) {
        base_y = -container_extents.y + spacing * 0.5f;
    }

    positions_.reserve(static_cast<size_t>(cap));
    bool full = false;
    for (int iz = 0; iz < nz && !full; ++iz) {
        for (int iy = 0; iy < ny && !full; ++iy) {
            for (int ix = 0; ix < nx; ++ix) {
                if (static_cast<int>(positions_.size()) >= cap) {
                    full = true;
                    break;
                }
                positions_.emplace_back(-spawn_extents.x + (static_cast<float>(ix) + 0.5f) * dx,
                                        base_y + (static_cast<float>(iy) + 0.5f) * dx,
                                        -spawn_extents.z + (static_cast<float>(iz) + 0.5f) * dx);
            }
        }
    }

    positions_prev_ = positions_;
    velocities_.assign(positions_.size(), math::Vector3f::zero());
    densities_.assign(positions_.size(), 1.0f);
    lambdas_.assign(positions_.size(), 0.0f);
    accumulator_ = 0.0f;
    gpu_dirty_ = true;
}

void SPHFluid3D::splash(const math::Vector3f& local_center, float radius, float strength) {
    const float r2 = std::max(1e-4f, radius * radius);
    for (size_t i = 0; i < positions_.size(); ++i) {
        const math::Vector3f d = positions_[i] - local_center;
        const float dist2 = d.length_sq();
        if (dist2 > r2) continue;
        const float w = 1.0f - dist2 / r2;
        velocities_[i] = velocities_[i] + d.normalized() * (strength * w) +
                         math::Vector3f(0.0f, strength * 0.5f * w, 0.0f);
    }
}

void SPHFluid3D::on_update(float dt) {
    if (reset_requested) {
        reset_requested = false;
        reset_particles();
    }
    // 参数变化可能改变粒子上限
    if (static_cast<int>(positions_.size()) > max_particles) {
        const size_t n = static_cast<size_t>(std::max(1, max_particles));
        positions_.resize(n);
        velocities_.resize(n, math::Vector3f::zero());
        positions_prev_.resize(n, math::Vector3f::zero());
        densities_.resize(n, 1.0f);
        lambdas_.resize(n, 0.0f);
        gpu_dirty_ = true;
    }
    if (!enabled || !simulate || dt <= 0.0f || positions_.empty()) return;

    accumulator_ += std::min(dt, 1.0f / 30.0f);
    int steps = 0;
    while (accumulator_ >= k_fixed_dt && steps < k_max_substeps) {
        simulate_step(k_fixed_dt);
        accumulator_ -= k_fixed_dt;
        ++steps;
    }
    // 落后太多就丢弃余量，避免越跑越慢（慢机器上的"追帧死亡螺旋"）
    if (steps == k_max_substeps) accumulator_ = 0.0f;
    gpu_dirty_ = true;
}

// ---------------------------------------------------------------------------
// 模拟
// ---------------------------------------------------------------------------

void SPHFluid3D::simulate_step(float dt) {
    if (positions_.empty()) return;
    // PBF（Macklin & Müller 2013）：先按外力预测位置，再用密度约束把位置
    // 投影回不可压流形，最后从位置差反推速度。相比显式压力力（Müller 2003），
    // 它在 1/60 步长下无条件稳定，也不会出现"内密度长期高于静止密度 → 水体
    // 持续膨胀 → 满屏飞沫"的自激振荡。
    predict(dt);
    build_grid();

    const int iterations = std::clamp(solver_iterations, 1, 8);
    for (int it = 0; it < iterations; ++it) {
        solve_density_constraints();
    }

    update_velocities(dt);
    collide_walls();
    apply_viscosity();
}

// 均匀网格 + 计数排序：把粒子按格分桶到连续数组，27 邻域遍历只剩顺序读。
// 相比 unordered_map<uint64,int>（旧实现）少了两级指针追逐与桶链，密度高时
// 的缓存缺失大幅下降。
void SPHFluid3D::build_grid() {
    const size_t n = positions_.size();
    const float h = smooth_radius();
    grid_cell_ = h;

    const bool has_container = container_extents.x > 0.0f && container_extents.y > 0.0f &&
                               container_extents.z > 0.0f;
    math::Vector3f lo;
    math::Vector3f hi;
    if (has_container) {
        lo = math::Vector3f(-container_extents.x, -container_extents.y, -container_extents.z);
        hi = container_extents;
    } else {
        accumulate_bounds();
        lo = bounds_min_ - math::Vector3f(h, h, h);
        hi = bounds_max_ + math::Vector3f(h, h, h);
    }

    grid_origin_ = lo;
    const float sx = std::max(1e-4f, hi.x - lo.x);
    const float sy = std::max(1e-4f, hi.y - lo.y);
    const float sz = std::max(1e-4f, hi.z - lo.z);
    grid_dim_[0] = std::clamp(static_cast<int>(std::ceil(sx / h)), 1, k_grid_max_dim);
    grid_dim_[1] = std::clamp(static_cast<int>(std::ceil(sy / h)), 1, k_grid_max_dim);
    grid_dim_[2] = std::clamp(static_cast<int>(std::ceil(sz / h)), 1, k_grid_max_dim);

    const int cells = grid_dim_[0] * grid_dim_[1] * grid_dim_[2];
    cell_start_.assign(static_cast<size_t>(cells) + 1, 0);
    cell_of_.resize(n);

    const float inv = 1.0f / h;
    for (size_t i = 0; i < n; ++i) {
        const math::Vector3f& p = positions_[i];
        const int cx = std::clamp(static_cast<int>((p.x - lo.x) * inv), 0, grid_dim_[0] - 1);
        const int cy = std::clamp(static_cast<int>((p.y - lo.y) * inv), 0, grid_dim_[1] - 1);
        const int cz = std::clamp(static_cast<int>((p.z - lo.z) * inv), 0, grid_dim_[2] - 1);
        const int idx = (cz * grid_dim_[1] + cy) * grid_dim_[0] + cx;
        cell_of_[i] = idx;
        ++cell_start_[static_cast<size_t>(idx) + 1];
    }
    for (int c = 0; c < cells; ++c) {
        cell_start_[static_cast<size_t>(c) + 1] += cell_start_[static_cast<size_t>(c)];
    }

    sorted_.resize(n);
    cell_cursor_.assign(cell_start_.begin(), cell_start_.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        sorted_[static_cast<size_t>(cell_cursor_[static_cast<size_t>(cell_of_[i])]++)] =
            static_cast<int>(i);
    }
}

void SPHFluid3D::accumulate_bounds() {
    if (positions_.empty()) {
        bounds_min_ = math::Vector3f::zero();
        bounds_max_ = math::Vector3f::zero();
        return;
    }
    bounds_min_ = positions_[0];
    bounds_max_ = positions_[0];
    for (const auto& p : positions_) {
        bounds_min_.x = std::min(bounds_min_.x, p.x);
        bounds_min_.y = std::min(bounds_min_.y, p.y);
        bounds_min_.z = std::min(bounds_min_.z, p.z);
        bounds_max_.x = std::max(bounds_max_.x, p.x);
        bounds_max_.y = std::max(bounds_max_.y, p.y);
        bounds_max_.z = std::max(bounds_max_.z, p.z);
    }
}

// 外力积分 + 位置预测。密度约束求解期间的"前一帧位置"存 positions_prev_，
// 用于最后反推速度。
void SPHFluid3D::predict(float dt) {
    const float buoy = std::max(0.0f, buoyancy_strength);
    const float max_step = grid_cell_ > 0.0f ? grid_cell_ * 0.4f : spacing * 0.4f;

    for (size_t i = 0; i < positions_.size(); ++i) {
        math::Vector3f acc = gravity +
                             (flow_velocity - velocities_[i]) * (linear_drag * 60.0f);
        // 密度差浮力：低于静止密度的粒子（气泡/浪尖）向上加速
        const float deficit = std::clamp(1.0f - densities_[i], 0.0f, 1.0f);
        acc = acc + math::Vector3f(0.0f, buoy * deficit, 0.0f);

        math::Vector3f v = velocities_[i] + acc * dt;
        positions_prev_[i] = positions_[i];
        math::Vector3f step = v * dt;
        const float len = step.length();
        if (len > max_step && len > 1e-9f) step = step * (max_step / len);
        positions_[i] = positions_[i] + step;
        velocities_[i] = v;
    }
}

// 密度约束投影：C_i = ρ_i - 1（核已按 dx³ 归一化，满密度时 ρ ≈ 1），
// λ_i = -C_i / (Σ|∇C|² + ε)，然后 Δp_i = Σ_j (λ_i + λ_j) ∇W。
void SPHFluid3D::solve_density_constraints() {
    const size_t n = positions_.size();
    if (n == 0) return;

    const float h = smooth_radius();
    const SphKernel kern(h);
    const float h2 = kern.h2;
    const float dx3 = spacing * spacing * spacing;
    const float k_density = kern.poly6 * dx3;
    const float k_grad = kern.spiky * dx3;
    const float self = k_density * h2 * h2 * h2;

    const int nx = grid_dim_[0];
    const int ny = grid_dim_[1];
    const int nz = grid_dim_[2];
    const float inv = 1.0f / grid_cell_;
    constexpr float k_epsilon = 1e-4f;

    // 1) 密度与拉格朗日乘子
    lambdas_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const math::Vector3f pi = positions_[i];
        const int cx = std::clamp(static_cast<int>((pi.x - grid_origin_.x) * inv), 0, nx - 1);
        const int cy = std::clamp(static_cast<int>((pi.y - grid_origin_.y) * inv), 0, ny - 1);
        const int cz = std::clamp(static_cast<int>((pi.z - grid_origin_.z) * inv), 0, nz - 1);

        float density = self;
        float denom = 0.0f;
        for (int oz = -1; oz <= 1; ++oz) {
            const int jz = cz + oz;
            if (jz < 0 || jz >= nz) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                const int jy = cy + oy;
                if (jy < 0 || jy >= ny) continue;
                const int row = (jz * ny + jy) * nx;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int jx = cx + ox;
                    if (jx < 0 || jx >= nx) continue;
                    const int cell = row + jx;
                    const int begin = cell_start_[static_cast<size_t>(cell)];
                    const int end = cell_start_[static_cast<size_t>(cell) + 1];
                    for (int sidx = begin; sidx < end; ++sidx) {
                        const int j = sorted_[static_cast<size_t>(sidx)];
                        if (j == static_cast<int>(i)) continue;
                        const math::Vector3f d = pi - positions_[static_cast<size_t>(j)];
                        const float r2 = d.length_sq();
                        if (r2 >= h2) continue;
                        const float q = h2 - r2;
                        density += k_density * q * q * q;
                        const float r = std::sqrt(std::max(r2, 1e-12f));
                        const float g = k_grad * (h - r) * (h - r);
                        denom += g * g;
                    }
                }
            }
        }
        densities_[i] = density;
        const float constraint = density - 1.0f;
        // 只对"过密"用全强度，欠密（自由表面天然 ρ≈0.5）只做弱修正：
        // 全强度会把表面粒子往内猛拽，逐迭代在 0.5↔1.6 之间振荡，
        // 位置修正量被推到上限后整块水被打散（数值探针已验证）。
        const float lambda = -constraint / (denom + k_epsilon);
        lambdas_[i] = (constraint < 0.0f) ? lambda * 0.2f : lambda;
    }

    // 2) 位置修正
    const float max_delta = spacing * 0.5f;
    for (size_t i = 0; i < n; ++i) {
        const math::Vector3f pi = positions_[i];
        const int cx = std::clamp(static_cast<int>((pi.x - grid_origin_.x) * inv), 0, nx - 1);
        const int cy = std::clamp(static_cast<int>((pi.y - grid_origin_.y) * inv), 0, ny - 1);
        const int cz = std::clamp(static_cast<int>((pi.z - grid_origin_.z) * inv), 0, nz - 1);

        math::Vector3f delta = math::Vector3f::zero();
        for (int oz = -1; oz <= 1; ++oz) {
            const int jz = cz + oz;
            if (jz < 0 || jz >= nz) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                const int jy = cy + oy;
                if (jy < 0 || jy >= ny) continue;
                const int row = (jz * ny + jy) * nx;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int jx = cx + ox;
                    if (jx < 0 || jx >= nx) continue;
                    const int cell = row + jx;
                    const int begin = cell_start_[static_cast<size_t>(cell)];
                    const int end = cell_start_[static_cast<size_t>(cell) + 1];
                    for (int sidx = begin; sidx < end; ++sidx) {
                        const int j = sorted_[static_cast<size_t>(sidx)];
                        if (j == static_cast<int>(i)) continue;
                        const math::Vector3f d = pi - positions_[static_cast<size_t>(j)];
                        const float r2 = d.length_sq();
                        if (r2 >= h2) continue;
                        const float r = std::sqrt(std::max(r2, 1e-12f));
                        const float g = k_grad * (h - r) * (h - r);
                        const float lam = lambdas_[i] + lambdas_[static_cast<size_t>(j)];
                        delta = delta + d * (lam * g / r);
                    }
                }
            }
        }
        const float dl = delta.length();
        if (dl > max_delta) delta = delta * (max_delta / dl);
        positions_[i] = positions_[i] + delta;
    }
}

void SPHFluid3D::update_velocities(float dt) {
    if (dt <= 1e-9f) return;
    const float inv_dt = 1.0f / dt;
    const float max_speed = 12.0f;
    for (size_t i = 0; i < positions_.size(); ++i) {
        const math::Vector3f v = (positions_[i] - positions_prev_[i]) * inv_dt;
        const float sp = v.length();
        velocities_[i] = (sp > max_speed) ? v * (max_speed / sp) : v;
    }
}

// XSPH：速度向邻域平均混合（blend ≤ 0.5，无条件稳定）
void SPHFluid3D::apply_viscosity() {
    const float blend = std::clamp(viscosity, 0.0f, 1.0f) * 0.5f;
    const size_t n = positions_.size();
    if (blend <= 0.0f || n == 0) return;

    const SphKernel kern(smooth_radius());
    const float h2 = kern.h2;
    const int nx = grid_dim_[0];
    const int ny = grid_dim_[1];
    const int nz = grid_dim_[2];
    const float inv = 1.0f / grid_cell_;

    std::vector<math::Vector3f> smoothed(n);
    for (size_t i = 0; i < n; ++i) {
        const math::Vector3f pi = positions_[i];
        const int cx = std::clamp(static_cast<int>((pi.x - grid_origin_.x) * inv), 0, nx - 1);
        const int cy = std::clamp(static_cast<int>((pi.y - grid_origin_.y) * inv), 0, ny - 1);
        const int cz = std::clamp(static_cast<int>((pi.z - grid_origin_.z) * inv), 0, nz - 1);

        math::Vector3f vsum = math::Vector3f::zero();
        float wsum = 0.0f;
        for (int oz = -1; oz <= 1; ++oz) {
            const int jz = cz + oz;
            if (jz < 0 || jz >= nz) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                const int jy = cy + oy;
                if (jy < 0 || jy >= ny) continue;
                const int row = (jz * ny + jy) * nx;
                for (int ox = -1; ox <= 1; ++ox) {
                    const int jx = cx + ox;
                    if (jx < 0 || jx >= nx) continue;
                    const int cell = row + jx;
                    const int begin = cell_start_[static_cast<size_t>(cell)];
                    const int end = cell_start_[static_cast<size_t>(cell) + 1];
                    for (int s = begin; s < end; ++s) {
                        const int j = sorted_[static_cast<size_t>(s)];
                        if (j == static_cast<int>(i)) continue;
                        const math::Vector3f d = pi - positions_[static_cast<size_t>(j)];
                        const float w = poly6_value(kern, d.length_sq());
                        if (w <= 0.0f) continue;
                        vsum = vsum + velocities_[static_cast<size_t>(j)] * w;
                        wsum += w;
                    }
                }
            }
        }
        smoothed[i] = (wsum > 1e-9f) ? vsum * (1.0f / wsum) : velocities_[i];
    }
    for (size_t i = 0; i < n; ++i) {
        velocities_[i] = velocities_[i] + (smoothed[i] - velocities_[i]) * blend;
    }
}

void SPHFluid3D::collide_walls() {
    if (container_extents.x <= 0.0f || container_extents.y <= 0.0f ||
        container_extents.z <= 0.0f) {
        return;
    }
    const float bounce = std::clamp(1.0f - wall_damping, 0.0f, 1.0f);
    const float margin = spacing * 0.5f;
    const float ex = std::max(0.01f, container_extents.x - margin);
    const float ey = std::max(0.01f, container_extents.y - margin);
    const float ez = std::max(0.01f, container_extents.z - margin);

    for (size_t i = 0; i < positions_.size(); ++i) {
        math::Vector3f& p = positions_[i];
        math::Vector3f& v = velocities_[i];
        if (p.x < -ex) { p.x = -ex; if (v.x < 0.0f) v.x = -v.x * bounce; }
        else if (p.x > ex) { p.x = ex; if (v.x > 0.0f) v.x = -v.x * bounce; }
        if (p.y < -ey) { p.y = -ey; if (v.y < 0.0f) v.y = -v.y * bounce; }
        else if (p.y > ey) { p.y = ey; if (v.y > 0.0f) v.y = -v.y * bounce; }
        if (p.z < -ez) { p.z = -ez; if (v.z < 0.0f) v.z = -v.z * bounce; }
        else if (p.z > ez) { p.z = ez; if (v.z > 0.0f) v.z = -v.z * bounce; }
    }
}

// ---------------------------------------------------------------------------
// 表面重建（Surface Nets）
// ---------------------------------------------------------------------------

int SPHFluid3D::resolve_surface_resolution() const {
    if (surface_resolution > 0) {
        return std::clamp(surface_resolution, k_surface_min_res, k_surface_max_res);
    }
    // 自动：格距取半个静止间距。等值面台阶的宽度就等于格距，缩到间距一半
    // 时水面边缘的锯齿已经落在单像素量级；全场格子数仍在 5 万上下。
    const float max_ext = std::max(container_extents.x,
                                   std::max(container_extents.y, container_extents.z));
    const float extent = (max_ext > 0.0f) ? max_ext * 2.0f
                                          : std::max(bounds_max_.x - bounds_min_.x,
                                                     std::max(bounds_max_.y - bounds_min_.y,
                                                              bounds_max_.z - bounds_min_.z)) +
                                                2.0f * smooth_radius();
    const float cell = std::max(1e-3f, spacing * 0.55f);
    return std::clamp(static_cast<int>(std::ceil(std::max(1e-3f, extent) / cell)),
                      k_surface_min_res, k_surface_max_res);
}

// 把粒子 splat 到规则网格角点：f(x) = Σ W_poly6(|x-p|) * dx³，
// 满密度内部 f ≈ 1（与密度归一化同一推理），等值面取 surface_iso。
void SPHFluid3D::build_surface_field() {
    const SphKernel kern(smooth_radius());
    const float h = kern.h;
    const float h2 = kern.h2;
    const float weight = spacing * spacing * spacing * kern.poly6;

    const bool has_container = container_extents.x > 0.0f && container_extents.y > 0.0f &&
                               container_extents.z > 0.0f;
    math::Vector3f lo;
    math::Vector3f hi;
    if (has_container) {
        // 容器范围外留一圈空白场：粒子在壁面处密度本就衰减，等值面在场里
        // 自然收口成圆边；若把场边界正好压在容器壁上，等值面会被硬截断成
        // 沿格线对齐的台阶（水边缘一圈方块状缺口）。
        const float pad = h * 0.75f;
        lo = math::Vector3f(-container_extents.x, -container_extents.y, -container_extents.z) -
             math::Vector3f(pad, pad, pad);
        hi = container_extents + math::Vector3f(pad, pad, pad);
    } else {
        accumulate_bounds();
        const float pad = h;
        lo = bounds_min_ - math::Vector3f(pad, pad, pad);
        hi = bounds_max_ + math::Vector3f(pad, pad, pad);
    }

    const int res = resolve_surface_resolution();
    const float max_ext = std::max(hi.x - lo.x, std::max(hi.y - lo.y, hi.z - lo.z));
    const float cell = std::max(1e-3f, max_ext / static_cast<float>(res));
    field_cell_ = cell;
    field_origin_ = lo;
    field_dim_[0] = std::clamp(static_cast<int>(std::ceil((hi.x - lo.x) / cell)), 1, res);
    field_dim_[1] = std::clamp(static_cast<int>(std::ceil((hi.y - lo.y) / cell)), 1, res);
    field_dim_[2] = std::clamp(static_cast<int>(std::ceil((hi.z - lo.z) / cell)), 1, res);

    const int cx = field_dim_[0];
    const int cy = field_dim_[1];
    const int cz = field_dim_[2];
    const int nx = cx + 1;
    const int ny = cy + 1;
    field_.assign(static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(cz + 1),
                  0.0f);

    // splat：每颗粒子只写 h 半径覆盖的角点（半径内 8 个角点插值）
    const int reach = std::max(1, static_cast<int>(std::ceil(h / cell)));
    for (const auto& p : positions_) {
        const int ix0 = static_cast<int>(std::floor((p.x - field_origin_.x) / cell));
        const int iy0 = static_cast<int>(std::floor((p.y - field_origin_.y) / cell));
        const int iz0 = static_cast<int>(std::floor((p.z - field_origin_.z) / cell));
        for (int oz = iz0 - reach; oz <= iz0 + reach; ++oz) {
            if (oz < 0 || oz > cz) continue;
            const float wz = p.z - (field_origin_.z + static_cast<float>(oz) * cell);
            for (int oy = iy0 - reach; oy <= iy0 + reach; ++oy) {
                if (oy < 0 || oy > cy) continue;
                const float wy = p.y - (field_origin_.y + static_cast<float>(oy) * cell);
                const int row = (oz * ny + oy) * nx;
                for (int ox = ix0 - reach; ox <= ix0 + reach; ++ox) {
                    if (ox < 0 || ox > cx) continue;
                    const float wx = p.x - (field_origin_.x + static_cast<float>(ox) * cell);
                    const float r2 = wx * wx + wy * wy + wz * wz;
                    if (r2 >= h2) continue;
                    const float q = h2 - r2;
                    field_[static_cast<size_t>(row + ox)] += weight * q * q * q;
                }
            }
        }
    }

    cell_vertex_.assign(static_cast<size_t>(cx) * cy * cz, -1);
}

float SPHFluid3D::sample_field_trilinear(float gx, float gy, float gz) const {
    const int cx = field_dim_[0];
    const int cy = field_dim_[1];
    const int cz = field_dim_[2];
    const int nx = cx + 1;
    const int ny = cy + 1;
    const auto at = [&](int x, int y, int z) {
        x = std::clamp(x, 0, cx);
        y = std::clamp(y, 0, cy);
        z = std::clamp(z, 0, cz);
        return field_[static_cast<size_t>((z * ny + y) * nx + x)];
    };
    gx = std::clamp(gx, 0.0f, static_cast<float>(cx));
    gy = std::clamp(gy, 0.0f, static_cast<float>(cy));
    gz = std::clamp(gz, 0.0f, static_cast<float>(cz));
    const int x0 = std::min(static_cast<int>(gx), cx - 1);
    const int y0 = std::min(static_cast<int>(gy), cy - 1);
    const int z0 = std::min(static_cast<int>(gz), cz - 1);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);
    const float fz = gz - static_cast<float>(z0);
    const float c000 = at(x0, y0, z0);
    const float c100 = at(x0 + 1, y0, z0);
    const float c010 = at(x0, y0 + 1, z0);
    const float c110 = at(x0 + 1, y0 + 1, z0);
    const float c001 = at(x0, y0, z0 + 1);
    const float c101 = at(x0 + 1, y0, z0 + 1);
    const float c011 = at(x0, y0 + 1, z0 + 1);
    const float c111 = at(x0 + 1, y0 + 1, z0 + 1);
    const float c00 = c000 + (c100 - c000) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

// 顶点法线 = -∇f，在顶点的真实位置（格坐标，可带小数）用中心差分求。
// 直接取单元角点梯度会让相邻单元法线在公共边跳变，水面上表现为网格状接缝。
math::Vector3f SPHFluid3D::surface_gradient_at(float gx, float gy, float gz) const {
    constexpr float k_step = 0.5f;
    const float dx = sample_field_trilinear(gx + k_step, gy, gz) -
                     sample_field_trilinear(gx - k_step, gy, gz);
    const float dy = sample_field_trilinear(gx, gy + k_step, gz) -
                     sample_field_trilinear(gx, gy - k_step, gz);
    const float dz = sample_field_trilinear(gx, gy, gz + k_step) -
                     sample_field_trilinear(gx, gy, gz - k_step);
    return math::Vector3f(dx, dy, dz);
}

// Nameless surface nets：每个跨越等值面的单元产出一个顶点（12 条边交点的
// 平均），再沿三条网格边把相邻 4 个单元的顶点连成四边形。
void SPHFluid3D::extract_surface(assets::MeshData& out) const {
    out.vertices.clear();
    out.indices.clear();
    const int cx = field_dim_[0];
    const int cy = field_dim_[1];
    const int cz = field_dim_[2];
    if (cx <= 0 || cy <= 0 || cz <= 0) return;

    const int nx = cx + 1;
    const int ny = cy + 1;
    const float cell = field_cell_;
    const float iso = surface_iso;
    const auto at = [&](int x, int y, int z) {
        return field_[static_cast<size_t>((z * ny + y) * nx + x)];
    };

    // 12 条边的端点（角点编号 c = x + 2y + 4z）
    static constexpr int k_edge[12][2] = {
        {0, 1}, {2, 3}, {4, 5}, {6, 7},   // along x
        {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along y
        {0, 4}, {1, 5}, {2, 6}, {3, 7}    // along z
    };

    out.vertices.reserve(static_cast<size_t>(cx) * 4);
    for (int z = 0; z < cz; ++z) {
        for (int y = 0; y < cy; ++y) {
            for (int x = 0; x < cx; ++x) {
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
                    const bool ia = v[a] > iso;
                    const bool ib = v[b] > iso;
                    if (ia == ib) continue;
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
                assets::MeshVertex vert;
                vert.position = math::Vector3f(field_origin_.x + (static_cast<float>(x) + local.x) * cell,
                                               field_origin_.y + (static_cast<float>(y) + local.y) * cell,
                                               field_origin_.z + (static_cast<float>(z) + local.z) * cell);
                // 法线 = -梯度（场在内部更大，梯度指向内部）。梯度在顶点的
                // 真实位置做三线性采样：直接取单元角点的梯度会让相邻单元的法线
                // 在公共边处跳变，低粗糙度水面上表现为网格状高光接缝。
                const math::Vector3f g = surface_gradient_at(
                    static_cast<float>(x) + local.x, static_cast<float>(y) + local.y,
                    static_cast<float>(z) + local.z);
                const float gl = g.length();
                vert.normal = (gl > 1e-9f) ? (g * (-1.0f / gl)) : math::Vector3f(0.0f, 1.0f, 0.0f);
                vert.tangent = math::Vector3f::right();
                // 顶点色必须是白：着色器里 albedo *= vColor，MeshVertex 的默认
                // 构造不初始化该字段，留空会把整片水面乘成黑色。
                vert.color = math::Vector3f(1.0f, 1.0f, 1.0f);
                vert.uv = math::Vector2f(vert.position.x, vert.position.z);

                cell_vertex_[static_cast<size_t>((z * cy + y) * cx + x)] =
                    static_cast<int>(out.vertices.size());
                out.vertices.push_back(vert);
            }
        }
    }

    if (out.vertices.empty()) return;

    const auto cell_at = [&](int x, int y, int z) {
        return cell_vertex_[static_cast<size_t>((z * cy + y) * cx + x)];
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

    for (int z = 1; z < cz; ++z) {
        for (int y = 1; y < cy; ++y) {
            for (int x = 0; x < cx; ++x) {
                // +X 边：角点 (x,y,z)-(x+1,y,z)，共享它的 4 个单元
                const float v0 = at(x, y, z);
                const float v1 = at(x + 1, y, z);
                if ((v0 > iso) != (v1 > iso)) {
                    emit_quad(cell_at(x, y - 1, z - 1), cell_at(x, y, z - 1),
                              cell_at(x, y, z), cell_at(x, y - 1, z));
                }
            }
        }
    }
    for (int z = 1; z < cz; ++z) {
        for (int y = 0; y < cy; ++y) {
            for (int x = 1; x < cx; ++x) {
                // +Y 边
                const float v0 = at(x, y, z);
                const float v1 = at(x, y + 1, z);
                if ((v0 > iso) != (v1 > iso)) {
                    emit_quad(cell_at(x - 1, y, z - 1), cell_at(x - 1, y, z),
                              cell_at(x, y, z), cell_at(x, y, z - 1));
                }
            }
        }
    }
    for (int z = 0; z < cz; ++z) {
        for (int y = 1; y < cy; ++y) {
            for (int x = 1; x < cx; ++x) {
                // +Z 边
                const float v0 = at(x, y, z);
                const float v1 = at(x, y, z + 1);
                if ((v0 > iso) != (v1 > iso)) {
                    emit_quad(cell_at(x - 1, y - 1, z), cell_at(x, y - 1, z),
                              cell_at(x, y, z), cell_at(x - 1, y, z));
                }
            }
        }
    }

    // 统一绕序：Surface Nets 只保证拓扑正确，四边形的绕序取决于等值面朝向，
    // 逐条边生成的顺序必然一半反一半正。two_sided 材质的着色器按 gl_FrontFacing
    // 翻转法线，绕序不一致会让一半面被翻成背面 → 整片水面发暗、并沿单元边界
    // 出现网格状接缝。这里按顶点法线（由场梯度得到，恒指向外部）逐三角形定向。
    for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        const uint32_t i0 = out.indices[t];
        const uint32_t i1 = out.indices[t + 1];
        const uint32_t i2 = out.indices[t + 2];
        const math::Vector3f& p0 = out.vertices[i0].position;
        const math::Vector3f& p1 = out.vertices[i1].position;
        const math::Vector3f& p2 = out.vertices[i2].position;
        const math::Vector3f u = p1 - p0;
        const math::Vector3f v = p2 - p0;
        const math::Vector3f face(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                                  u.x * v.y - u.y * v.x);
        const math::Vector3f nref = out.vertices[i0].normal + out.vertices[i1].normal +
                                    out.vertices[i2].normal;
        if (face.x * nref.x + face.y * nref.y + face.z * nref.z < 0.0f) {
            out.indices[t + 1] = i2;
            out.indices[t + 2] = i1;
        }
    }

    out.name = "SPHFluidSurface";
}

// ---------------------------------------------------------------------------
// GPU 上传
// ---------------------------------------------------------------------------

void SPHFluid3D::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    queued_generation_.store(0, std::memory_order_release);
}

void SPHFluid3D::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                              float& radius) const {
    const bool has_container = container_extents.x > 0.0f && container_extents.y > 0.0f &&
                               container_extents.z > 0.0f;
    const math::Vector3f ext = has_container
                                   ? container_extents
                                   : math::Vector3f(
                                         std::max(std::abs(bounds_min_.x), std::abs(bounds_max_.x)),
                                         std::max(std::abs(bounds_min_.y), std::abs(bounds_max_.y)),
                                         std::max(std::abs(bounds_min_.z), std::abs(bounds_max_.z)));
    center = world.transform_point(math::Vector3f::zero());
    const float local_radius = ext.length();
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = local_radius * std::max(sx, std::max(sy, sz));
}

void SPHFluid3D::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && material_) material_->destroy_gpu(ctx_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        queued_generation_.store(0, std::memory_order_release);
        material_uploaded_.store(false, std::memory_order_release);
        ctx_ = ctx;
        gpu_dirty_ = true;
    }

    material_->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
    material_->opacity = water_color.w;
    material_->roughness = roughness;
    material_->metallic = metallic;

    ++frame_counter_;
    const int interval = std::clamp(surface_update_interval, 1, 8);
    const bool rebuild = gpu_dirty_ && (frame_counter_ % interval == 0 || !geometry_);

    if (rebuild) {
        build_surface_field();
        auto mesh = std::make_shared<assets::MeshData>();
        extract_surface(*mesh);
        gpu_dirty_ = false;
        if (mesh->empty()) {
            // 没有可提取的表面（粒子散开/尚未稳定）：保留上一帧网格
            if (!geometry_) return;
        } else {
            geometry_ = std::move(mesh);
            ++geometry_generation_;
        }
    }
    if (!geometry_ || geometry_->empty()) return;

    const uint64_t generation = geometry_generation_;
    const bool need_geometry = generation != queued_generation_.load(std::memory_order_acquire);
    const bool need_material = material_ && !material_uploaded_.load(std::memory_order_acquire);
    if (!need_geometry && !need_material) return;

    auto geometry = geometry_;
    if (need_geometry) queued_generation_.store(generation, std::memory_order_release);

    ctx->push_command([this, ctx, geometry, need_geometry, token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (need_geometry) {
            if (!gpu_mesh_handle_.is_valid()) {
                gpu_mesh_handle_ = ctx->create_mesh();
                render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
                if (!mesh) {
                    gpu_mesh_handle_ = render::RHIMeshHandle{};
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

            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (mesh) {
                // 表面网格顶点数逐帧变化（水面形状在变），每帧整段重传
                const uint32_t vcount = static_cast<uint32_t>(geometry->vertices.size());
                const uint32_t icount = static_cast<uint32_t>(geometry->indices.size());
                mesh->upload_vertices(geometry->vertices.data(),
                                      static_cast<uint32_t>(vcount * sizeof(assets::MeshVertex)),
                                      vcount);
                mesh->upload_indices(geometry->indices.data(),
                                     static_cast<uint32_t>(icount * sizeof(uint32_t)), icount);
            }
        }

        if (material_ && !material_uploaded_.exchange(true, std::memory_order_acq_rel)) {
            material_->upload_to_gpu(ctx);
        }
    });
}

} // namespace gryce_engine::components
