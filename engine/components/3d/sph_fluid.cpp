#include "components/3d/sph_fluid.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "components/3d/particle_system_3d.h"
#include "render/material.h"
#include "render/mesh.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// Müller 2003 核函数（3D）：
// poly6  W(r,h)   = 315/(64 π h^9) (h²-r²)³          （密度）
// spiky  ∇W(r,h)  = -45/(π h^6) (h-r)² * (r_vec/r)   （压力梯度）
// visc   ∇²W(r,h) = 45/(π h^6) (h-r)                 （粘度）
constexpr float k_pi = 3.14159265358979323846f;

struct SphKernel {
    float h = 0.12f;
    float h2 = 0.0f;
    float poly6 = 0.0f;        // 315/(64 π h^9)
    float spiky = 0.0f;        // -45/(π h^6)
    float visc = 0.0f;         // 45/(π h^6)

    explicit SphKernel(float radius) : h(radius) {
        h2 = h * h;
        const float h6 = h2 * h2 * h2;
        const float h9 = h6 * h2 * h;
        poly6 = 315.0f / (64.0f * k_pi * h9);
        spiky = -45.0f / (k_pi * h6);
        visc = 45.0f / (k_pi * h6);
    }
};

// 三维 cell 坐标打包成 64 位 key（每轴 21 位，偏移到非负）
inline uint64_t cell_key(int cx, int cy, int cz) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx + 1048576)) << 42) |
           (static_cast<uint64_t>(static_cast<uint32_t>(cy + 1048576)) << 21) |
           static_cast<uint64_t>(static_cast<uint32_t>(cz + 1048576));
}

} // namespace

SPHFluid3D::SPHFluid3D() {
    for (auto& slot : scratch_) {
        slot = std::make_shared<std::vector<FluidInstance>>();
    }
    reset_particles();
}

SPHFluid3D::~SPHFluid3D() {
    *alive_token_ = false;
    if (ctx_) {
        if (gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (texture_handle_.is_valid()) ctx_->destroy_texture(texture_handle_);
    }
}

void SPHFluid3D::on_destroy() {
    *alive_token_ = false;
}

void SPHFluid3D::reset_particles() {
    positions_.clear();
    velocities_.clear();
    forces_.clear();
    densities_.clear();
    pressures_.clear();

    // 平滑半径取 2 倍间距：静止堆叠时每粒约 30 个邻居，3D SPH 的常用配置
    spacing = std::max(0.005f, spacing);
    h_ = spacing * 2.0f;

    const float dx = spacing;
    const int nx = std::max(1, static_cast<int>(spawn_extents.x * 2.0f / dx));
    const int ny = std::max(1, static_cast<int>(spawn_extents.y * 2.0f / dx));
    const int nz = std::max(1, static_cast<int>(spawn_extents.z * 2.0f / dx));
    const int cap = std::max(1, max_particles);

    positions_.reserve(static_cast<size_t>(cap));
    for (int iz = 0; iz < nz; ++iz) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int ix = 0; ix < nx; ++ix) {
                if (static_cast<int>(positions_.size()) >= cap) break;
                positions_.emplace_back(-spawn_extents.x + (static_cast<float>(ix) + 0.5f) * dx,
                                        -spawn_extents.y + (static_cast<float>(iy) + 0.5f) * dx,
                                        -spawn_extents.z + (static_cast<float>(iz) + 0.5f) * dx);
            }
            if (static_cast<int>(positions_.size()) >= cap) break;
        }
        if (static_cast<int>(positions_.size()) >= cap) break;
    }

    velocities_.assign(positions_.size(), math::Vector3f::zero());
    forces_.assign(positions_.size(), math::Vector3f::zero());
    densities_.assign(positions_.size(), rest_density);
    pressures_.assign(positions_.size(), 0.0f);
    accumulator_ = 0.0f;
}

void SPHFluid3D::on_update(float dt) {
    if (reset_requested) {
        reset_requested = false;
        reset_particles();
    }
    // 参数变化可能改变了粒子总量上限
    if (static_cast<int>(positions_.size()) > max_particles) {
        positions_.resize(static_cast<size_t>(max_particles));
        velocities_.resize(positions_.size());
        forces_.resize(positions_.size());
        densities_.resize(positions_.size());
        pressures_.resize(positions_.size());
    }

    if (enabled && simulate && dt > 0.0f) {
        static bool s_dbg_sim = false;
        if (!s_dbg_sim) {
            s_dbg_sim = true;
            GLOG_INFO("[dbg-sph] on_update dt={} n={} h={}", dt, positions_.size(), h_);
        }
        // 固定子步长：SPH 对步长敏感（CFL），帧时长拆成不超过 1/120s 的子步，
        // 单帧最多 4 步防卡死放大
        accumulator_ += std::min(dt, 1.0f / 30.0f);
        int steps = 0;
        while (accumulator_ >= fixed_dt_ && steps < 4) {
            simulate_step(fixed_dt_);
            accumulator_ -= fixed_dt_;
            ++steps;
        }
        if (steps == 4) accumulator_ = 0.0f;
    }
}

void SPHFluid3D::simulate_step(float dt) {
    const size_t n = positions_.size();
    if (n == 0) return;

    build_neighbors();
    compute_density_pressure();
    compute_forces();
    integrate(dt);
    apply_viscosity();
    collide_walls();
}

void SPHFluid3D::build_neighbors() {
    const size_t n = positions_.size();
    // 均匀网格：格子边长 = h，查 27 邻格即可覆盖全部 h 半径邻居
    grid_map_.clear();
    grid_map_.reserve(n * 2);
    grid_cells_.clear();
    grid_origin_.assign(n, -1);

    for (size_t i = 0; i < n; ++i) {
        const math::Vector3f& p = positions_[i];
        const int cx = static_cast<int>(std::floor(p.x / h_));
        const int cy = static_cast<int>(std::floor(p.y / h_));
        const int cz = static_cast<int>(std::floor(p.z / h_));
        const uint64_t key = cell_key(cx, cy, cz);
        auto it = grid_map_.find(key);
        int cell;
        if (it == grid_map_.end()) {
            cell = static_cast<int>(grid_cells_.size());
            grid_map_.emplace(key, cell);
            grid_cells_.emplace_back();
        } else {
            cell = it->second;
        }
        grid_cells_[cell].push_back(static_cast<int>(i));
        grid_origin_[i] = cell;
    }
}

// 27 邻格遍历的公共骨架：对粒子 i 的每个邻居 j 调用 fn(dist_sq, j)
template <typename Fn>
inline void for_each_neighbor(std::unordered_map<uint64_t, int>& grid_map,
                              const std::vector<std::vector<int>>& cells,
                              const std::vector<math::Vector3f>& positions, int i, float h, Fn&& fn) {
    const math::Vector3f& pi = positions[static_cast<size_t>(i)];
    const int cx = static_cast<int>(std::floor(pi.x / h));
    const int cy = static_cast<int>(std::floor(pi.y / h));
    const int cz = static_cast<int>(std::floor(pi.z / h));
    const float h2 = h * h;

    for (int oz = -1; oz <= 1; ++oz) {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                auto it = grid_map.find(cell_key(cx + ox, cy + oy, cz + oz));
                if (it == grid_map.end()) continue;
                for (int j : cells[static_cast<size_t>(it->second)]) {
                    if (j == i) continue;
                    const math::Vector3f diff = pi - positions[static_cast<size_t>(j)];
                    const float r2 = diff.length_sq();
                    if (r2 < h2) fn(r2, j);
                }
            }
        }
    }
}

void SPHFluid3D::compute_density_pressure() {
    const size_t n = positions_.size();
    const SphKernel kern(h_);
    const float rest = std::max(1e-3f, rest_density);
    // 粒子质量：静止堆叠（间距 dx = h/2）时 poly6 归一化给出 Σ W ≈ 1/dx³，
    // 乘上 m = ρ0·dx³ 后静止密度恰好落在 ρ0 附近（Müller 推导的离散一致性）
    const float particle_mass = rest * spacing * spacing * spacing;

    for (size_t i = 0; i < n; ++i) {
        // 自身贡献（r=0 的 poly6：h² 内核值最大处）
        float density = particle_mass * kern.poly6 * kern.h2 * kern.h2 * kern.h2;
        for_each_neighbor(grid_map_, grid_cells_, positions_, static_cast<int>(i), h_,
                          [&](float r2, int /*j*/) {
                              const float diff2 = kern.h2 - r2;
                              density += particle_mass * kern.poly6 * diff2 * diff2 * diff2;
                          });
        densities_[i] = density;
        // 气体状态方程：p = k (ρ - ρ0)，负压截断为 0（吸引力是数值爆炸的主要来源）
        pressures_[i] = std::max(0.0f, stiffness * (density - rest));
    }
}

void SPHFluid3D::compute_forces() {
    const size_t n = positions_.size();
    const SphKernel kern(h_);
    const float rest = std::max(1e-3f, rest_density);
    const float buoy = std::max(0.0f, buoyancy_strength);
    const float particle_mass = rest * spacing * spacing * spacing;

    for (size_t i = 0; i < n; ++i) {
        math::Vector3f acc = gravity;

        // 均匀流场 + 线性阻力：推向与 flow_velocity 同速（对齐 BuoyancyArea2D）
        acc = acc + (flow_velocity - velocities_[i]) * linear_drag * 60.0f;

        // 密度差浮力：低于静止密度的粒子（气泡/泡沫）按密度缺口获得向上加速度
        const float deficit = std::clamp((rest - densities_[i]) / rest, 0.0f, 1.0f);
        acc = acc + math::Vector3f(0.0f, 1.0f, 0.0f) * (buoy * deficit);

        math::Vector3f pressure_acc = math::Vector3f::zero();

        const math::Vector3f& pi = positions_[i];

        for_each_neighbor(grid_map_, grid_cells_, positions_, static_cast<int>(i), h_,
                          [&](float r2, int j) {
                              const float r = std::sqrt(std::max(r2, 1e-12f));
                              if (r >= h_) return;
                              const size_t uj = static_cast<size_t>(j);
                              const math::Vector3f rij = pi - positions_[uj];

                              // 压力：spiky 梯度（压力已非负截断）
                              const float pterm = (pressures_[i] + pressures_[uj]) * 0.5f *
                                                  particle_mass * kern.spiky * (h_ - r) * (h_ - r);
                              pressure_acc = pressure_acc - rij * (pterm / (r * rest));
                          });

        // 粘度不进力项：Müller 显式粘度在小 h 下阻尼率 ∝ h^-6，dt×rate ≫ 1
        // 必然发散。粘度改为积分后的 XSPH 速度平滑（apply_viscosity_x sph）。

        // 压力/粘度项限幅：单条力项的病态放大（近距粒子 r→0）是 SPH 爆炸的主因
        constexpr float k_max_force = 400.0f;
        math::Vector3f f = pressure_acc;
        const float fl = f.length();
        if (fl > k_max_force) f = f * (k_max_force / fl);
        forces_[i] = f + acc;
    }
}

void SPHFluid3D::integrate(float dt) {
    const size_t n = positions_.size();
    const float max_speed = h_ / dt * 0.2f;   // CFL：单步位移不超过 0.2h
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(positions_[i].x) || !std::isfinite(positions_[i].y) ||
            !std::isfinite(positions_[i].z)) {
            // NaN 防护：重置到出生盒中心附近，避免污染扩散
            positions_[i] = math::Vector3f(0.0f, 0.0f, 0.0f);
            velocities_[i] = math::Vector3f::zero();
            continue;
        }
        math::Vector3f v = velocities_[i] + forces_[i] * dt;
        const float sp = v.length();
        if (sp > max_speed) v = v * (max_speed / sp);
        velocities_[i] = v;
        positions_[i] = positions_[i] + v * dt;
    }
}

// XSPH 粘度：把每粒速度向邻域平均速度混合。poly6 归一化保证混合系数有界，
// blend = clamp(viscosity,0,1) * 0.5 → 每子步最多修正 50%，无条件稳定。
void SPHFluid3D::apply_viscosity() {
    const size_t n = positions_.size();
    const float blend = std::clamp(viscosity, 0.0f, 1.0f) * 0.5f;
    if (blend <= 0.0f || n == 0) return;

    const SphKernel kern(h_);
    std::vector<math::Vector3f> smoothed(n);
    for (size_t i = 0; i < n; ++i) {
        math::Vector3f vsum = math::Vector3f::zero();
        float wsum = 0.0f;
        for_each_neighbor(grid_map_, grid_cells_, positions_, static_cast<int>(i), h_,
                          [&](float r2, int j) {
                              const float diff2 = kern.h2 - r2;
                              const float w = kern.poly6 * diff2 * diff2 * diff2;
                              vsum = vsum + velocities_[static_cast<size_t>(j)] * w;
                              wsum += w;
                          });
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
    const size_t n = positions_.size();
    const float bounce = std::clamp(1.0f - wall_damping, 0.0f, 1.0f);
    const float margin = spacing * 0.5f;

    for (size_t i = 0; i < n; ++i) {
        math::Vector3f& p = positions_[i];
        math::Vector3f& v = velocities_[i];
        const float ex = std::max(0.01f, container_extents.x - margin);
        const float ey = std::max(0.01f, container_extents.y - margin);
        const float ez = std::max(0.01f, container_extents.z - margin);

        if (p.x < -ex) { p.x = -ex; if (v.x < 0.0f) v.x = -v.x * bounce; }
        else if (p.x > ex) { p.x = ex; if (v.x > 0.0f) v.x = -v.x * bounce; }
        if (p.y < -ey) { p.y = -ey; if (v.y < 0.0f) v.y = -v.y * bounce; }
        else if (p.y > ey) { p.y = ey; if (v.y > 0.0f) v.y = -v.y * bounce; }
        if (p.z < -ez) { p.z = -ez; if (v.z < 0.0f) v.z = -v.z * bounce; }
        else if (p.z > ez) { p.z = ez; if (v.z > 0.0f) v.z = -v.z * bounce; }
    }
}

void SPHFluid3D::build_instances(const math::Matrix4f& model) {
    const uint32_t cap = static_cast<uint32_t>(std::max(1, max_particles));
    auto& buffer = *scratch_[scratch_index_];
    if (buffer.size() != cap) {
        buffer.assign(cap, FluidInstance{});
    }

    uint32_t w = 0;
    const float speed_norm = std::max(0.001f, spacing * 20.0f);
    for (size_t i = 0; i < positions_.size() && w < cap; ++i) {
        const math::Vector3f world = model.transform_point(positions_[i]);
        const float speed = velocities_[i].length();

        FluidInstance& inst = buffer[w];
        inst.px = world.x;
        inst.py = world.y;
        inst.pz = world.z;
        inst.size = particle_size;
        inst.rotation = 0.0f;
        inst.r = water_color.r;
        inst.g = water_color.g;
        inst.b = water_color.b;
        inst.a = water_color.a;
        if (speed_tint) {
            // 按速度向白色过渡：静止水体质感、浪尖泛白
            const float t = std::clamp(speed / speed_norm, 0.0f, 1.0f);
            inst.r += (1.0f - inst.r) * t;
            inst.g += (1.0f - inst.g) * t;
            inst.b += (1.0f - inst.b) * t;
        }
        ++w;
    }
    used_instances_ = w;
}

void SPHFluid3D::prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && texture_handle_.is_valid()) ctx_->destroy_texture(texture_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        texture_handle_ = render::RHITextureHandle{};
        gpu_ready_.store(false, std::memory_order_release);
        ctx_ = ctx;
    }

    build_instances(model);
    if (used_instances_ == 0) return;

    const uint32_t cap = static_cast<uint32_t>(std::max(1, max_particles));
    const bool size_buffer = !gpu_ready_.load(std::memory_order_acquire);
    const uint32_t upload_bytes =
        (size_buffer ? cap : used_instances_) * static_cast<uint32_t>(sizeof(FluidInstance));
    const uint32_t used = used_instances_;

    auto buffer = scratch_[scratch_index_];
    scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;

    ctx->push_command([this, ctx, buffer, upload_bytes, used,
                       token = alive_token_](render::IRenderBackend*) {
        if (!*token) return;

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 广告牌原型几何与实例流布局与 ParticleSystem3D 完全一致，
            // 复用同一份粒子着色器（binding 1 描述见 vulkan_particle.vert）
            static const float k_quad[6][2] = {
                {-1.0f, -1.0f}, { 1.0f, -1.0f}, { 1.0f,  1.0f},
                {-1.0f, -1.0f}, { 1.0f,  1.0f}, {-1.0f,  1.0f}
            };
            render::VertexLayout quad_layout;
            quad_layout.stride = sizeof(float) * 2;
            quad_layout.attributes = {
                {0, render::VertexType::Float2, false, 0}
            };
            mesh->set_layout(quad_layout);
            mesh->upload_vertices(k_quad, static_cast<uint32_t>(sizeof(k_quad)), 6);

            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(FluidInstance);
            instance_layout.attributes = {
                {1, render::VertexType::Float3, false, 0},
                {2, render::VertexType::Float,  false, 12},
                {3, render::VertexType::Float,  false, 16},
                {4, render::VertexType::Float4, false, 20}
            };
            mesh->set_instance_layout(instance_layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh && upload_bytes <= buffer->size() * sizeof(FluidInstance)) {
            mesh->upload_instances(buffer->data(), upload_bytes, used);
        }

        if (!texture_handle_.is_valid()) {
            texture_handle_ = ParticleSystem3D::create_default_texture(ctx);
        }

        gpu_ready_.store(true, std::memory_order_release);
    });
}

void SPHFluid3D::invalidate_gpu() {
    if (ctx_) {
        if (gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (texture_handle_.is_valid()) ctx_->destroy_texture(texture_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    texture_handle_ = render::RHITextureHandle{};
    gpu_ready_.store(false, std::memory_order_release);
}

void SPHFluid3D::serialize(nlohmann::json& out) const {
    out["spawn_extents"] = { spawn_extents.x, spawn_extents.y, spawn_extents.z };
    out["spacing"] = spacing;
    out["max_particles"] = max_particles;
    out["rest_density"] = rest_density;
    out["stiffness"] = stiffness;
    out["viscosity"] = viscosity;
    out["gravity"] = { gravity.x, gravity.y, gravity.z };
    out["buoyancy_strength"] = buoyancy_strength;
    out["linear_drag"] = linear_drag;
    out["flow_velocity"] = { flow_velocity.x, flow_velocity.y, flow_velocity.z };
    out["container_extents"] = { container_extents.x, container_extents.y, container_extents.z };
    out["wall_damping"] = wall_damping;
    out["particle_size"] = particle_size;
    out["water_color"] = { water_color.r, water_color.g, water_color.b, water_color.a };
    out["speed_tint"] = speed_tint;
    out["simulate"] = simulate;
}

void SPHFluid3D::deserialize(const nlohmann::json& in) {
    auto se = in.value("spawn_extents", std::vector<float>{0.4f, 0.4f, 0.4f});
    if (se.size() >= 3) spawn_extents = math::Vector3f(se[0], se[1], se[2]);
    spacing = in.value("spacing", 0.06f);
    max_particles = in.value("max_particles", 800);
    rest_density = in.value("rest_density", 1000.0f);
    stiffness = in.value("stiffness", 8.0f);
    viscosity = in.value("viscosity", 0.15f);
    auto g = in.value("gravity", std::vector<float>{0.0f, -9.8f, 0.0f});
    if (g.size() >= 3) gravity = math::Vector3f(g[0], g[1], g[2]);
    buoyancy_strength = in.value("buoyancy_strength", 6.0f);
    linear_drag = in.value("linear_drag", 0.08f);
    auto fv = in.value("flow_velocity", std::vector<float>{0.0f, 0.0f, 0.0f});
    if (fv.size() >= 3) flow_velocity = math::Vector3f(fv[0], fv[1], fv[2]);
    auto ce = in.value("container_extents", std::vector<float>{1.0f, 0.8f, 1.0f});
    if (ce.size() >= 3) container_extents = math::Vector3f(ce[0], ce[1], ce[2]);
    wall_damping = in.value("wall_damping", 0.3f);
    particle_size = in.value("particle_size", 0.09f);
    auto wc = in.value("water_color", std::vector<float>{0.25f, 0.55f, 0.95f, 0.85f});
    if (wc.size() >= 4) water_color = render::Color(wc[0], wc[1], wc[2], wc[3]);
    speed_tint = in.value("speed_tint", true);
    simulate = in.value("simulate", true);
}

} // namespace gryce_engine::components
