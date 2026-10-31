#include "components/3d/fluid_surface.h"

#include <algorithm>
#include <cmath>

#include "render/material.h"
#include "render/mesh.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// 浅水方程常数（世界尺度：米、秒）
constexpr float k_gravity = 9.81f;
// 每帧最多推进的物理时间：低于该值按慢动作处理（高分辨率下子步数受限）
constexpr float k_max_frame_dt = 1.0f / 30.0f;
constexpr int k_max_substeps = 12;
// 出流限幅安全系数：单步从一格流走的水量不得超过存量的 80%（否则抽干
// 出现负高度，下一步产生正反馈发散）
constexpr float k_outflow_safety = 0.8f;

} // namespace

FluidSurface3D::FluidSurface3D() {
    material_ = std::make_unique<render::Material>();
    material_->blend_mode = render::Material::BlendMode::Blend;
    material_->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
    material_->opacity = water_color.w;
}

FluidSurface3D::~FluidSurface3D() {
    // 使延迟执行的异步上传命令失效，避免渲染线程访问已析构的 this
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (material_) material_->destroy_gpu(ctx_);
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

render::Material* FluidSurface3D::ensure_material() {
    if (!material_) material_ = std::make_unique<render::Material>();
    return material_.get();
}

void FluidSurface3D::serialize(nlohmann::json& out) const {
    out["width"] = width;
    out["depth"] = depth;
    out["resolution"] = resolution;
    out["water_level"] = water_level;
    out["linear_drag"] = linear_drag;
    out["flow_velocity"] = {flow_velocity.x, flow_velocity.y, flow_velocity.z};
    out["rain_rate"] = rain_rate;
    out["rain_strength"] = rain_strength;
    out["water_color"] = {water_color.x, water_color.y, water_color.z, water_color.w};
    out["simulate"] = simulate;
}

void FluidSurface3D::deserialize(const nlohmann::json& in) {
    width = in.value("width", 2.0f);
    depth = in.value("depth", 2.0f);
    resolution = in.value("resolution", 96);
    water_level = in.value("water_level", 0.35f);
    linear_drag = in.value("linear_drag", 0.05f);
    if (in.contains("flow_velocity") && in["flow_velocity"].is_array()) {
        const auto& v = in["flow_velocity"];
        flow_velocity = math::Vector3f(v.at(0).get<float>(), v.at(1).get<float>(),
                                       v.at(2).get<float>());
    }
    rain_rate = in.value("rain_rate", 0.0f);
    rain_strength = in.value("rain_strength", 0.02f);
    if (in.contains("water_color") && in["water_color"].is_array()) {
        const auto& c = in["water_color"];
        water_color = math::Vector4f(c.at(0).get<float>(), c.at(1).get<float>(),
                                     c.at(2).get<float>(), c.at(3).get<float>());
    }
    simulate = in.value("simulate", true);

    material_->albedo_color = math::Vector3f(water_color.x, water_color.y, water_color.z);
    material_->opacity = water_color.w;
    ensure_fields();
    gpu_dirty_ = true;
}

void FluidSurface3D::ensure_fields() {
    const int n = stride() * stride();
    if (static_cast<int>(height_.size()) != n) {
        height_.assign(n, water_level);
        flux_l_.assign(n, 0.0f);
        flux_r_.assign(n, 0.0f);
        flux_t_.assign(n, 0.0f);
        flux_b_.assign(n, 0.0f);
    }
}

void FluidSurface3D::reset_to_rest() {
    height_.assign(stride() * stride(), water_level);
    flux_l_.assign(height_.size(), 0.0f);
    flux_r_.assign(height_.size(), 0.0f);
    flux_t_.assign(height_.size(), 0.0f);
    flux_b_.assign(height_.size(), 0.0f);
    gpu_dirty_ = true;
}

void FluidSurface3D::splash(float local_x, float local_z, float radius, float strength) {
    ensure_fields();
    const int s = stride();
    const float cell = width / static_cast<float>(resolution);
    const float half = width * 0.5f;
    const float cx = (local_x + half) / cell;
    const float cz = (local_z + half) / cell;
    const int r = std::max(1, static_cast<int>(radius / cell));
    for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) {
            const int x = static_cast<int>(cx) + dx;
            const int z = static_cast<int>(cz) + dz;
            if (x < 0 || x >= s || z < 0 || z >= s) continue;
            const float d2 = static_cast<float>(dx * dx + dz * dz) / static_cast<float>(r * r);
            if (d2 > 1.0f) continue;
            height_[z * s + x] += strength * (1.0f - d2);
        }
    }
    gpu_dirty_ = true;
}

void FluidSurface3D::on_update(float dt) {
    if (!enabled || !simulate || dt <= 0.0f) return;
    ensure_fields();

    // 子步长按库朗条件自适应：浅水波速 sqrt(g*h)，要求 dt*波速/格距 < 1。
    // 固定子步长在细网格（格距 ~1cm）下必爆：0.35m 水深波速 1.85m/s，
    // 1/120s 单步位移 1.5cm 已超格距。0.4 系数留裕度。
    const float cell = width / static_cast<float>(resolution);
    const float wave_speed = std::sqrt(k_gravity * (water_level + 0.5f));
    const float dt_max = std::clamp(0.4f * cell / wave_speed, 1.0f / 600.0f, 1.0f / 60.0f);

    float remaining = std::min(dt, k_max_frame_dt);
    int steps = 0;
    while (remaining > 1e-6f && steps < k_max_substeps) {
        const float step = std::min(dt_max, remaining);
        simulate_step(step);
        remaining -= step;
        ++steps;
    }
}

void FluidSurface3D::simulate_step(float dt) {
    const int s = stride();
    const float cell = width / static_cast<float>(resolution);
    const float cell_area = cell * cell;
    // 每秒阻尼比例 linear_drag → 单步保留系数
    const float damp = std::pow(std::clamp(1.0f - linear_drag, 0.0f, 1.0f), dt);
    // 流场倾斜驱动：flow_velocity 视作水平加速度（m/s²），等效于给重力一个
    // 水平分量；稳态水面坡度 = a/g（0.6 → 约 6% 斜率），水体被持续推向
    // flow 方向，在边界堆积后回涌形成涌浪
    const float tilt_x = std::clamp(flow_velocity.x, -50.0f, 50.0f);
    const float tilt_z = std::clamp(flow_velocity.z, -50.0f, 50.0f);

    // 1) 流量更新：管道两端的水位差驱动，重力把水位差转化为流量
    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            const int i = z * s + x;
            const float h = height_[i];
            // 四向出流：水位差经重力转成流量；倾斜项是独立的水平加速度
            //（不再乘 g），稳态坡度 = a/g，与物理一致
            float fl = 0.0f;
            if (x > 0) {
                fl = flux_l_[i] * damp -
                     dt * cell_area * (k_gravity * (height_[i - 1] - h) / cell + tilt_x);
            } else {
                fl = 0.0f;   // 封闭边界：无出流
            }
            float fr = 0.0f;
            if (x < s - 1) {
                fr = flux_r_[i] * damp +
                     dt * cell_area * (k_gravity * (h - height_[i + 1]) / cell + tilt_x);
            }
            float ft = 0.0f;
            if (z > 0) {
                ft = flux_t_[i] * damp -
                     dt * cell_area * (k_gravity * (height_[i - s] - h) / cell + tilt_z);
            }
            float fb = 0.0f;
            if (z < s - 1) {
                fb = flux_b_[i] * damp +
                     dt * cell_area * (k_gravity * (h - height_[i + s]) / cell + tilt_z);
            }
            flux_l_[i] = std::max(0.0f, fl);
            flux_r_[i] = std::max(0.0f, fr);
            flux_t_[i] = std::max(0.0f, ft);
            flux_b_[i] = std::max(0.0f, fb);

            // 出流限幅：总出流量不得超过当前存量的安全比例
            const float total_out = flux_l_[i] + flux_r_[i] + flux_t_[i] + flux_b_[i];
            const float max_out = height_[i] * cell_area * k_outflow_safety / dt;
            if (total_out > max_out && total_out > 1e-9f) {
                const float scale = max_out / total_out;
                flux_l_[i] *= scale;
                flux_r_[i] *= scale;
                flux_t_[i] *= scale;
                flux_b_[i] *= scale;
            }
        }
    }

    // 2) 高度更新：入流 - 出流
    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            const int i = z * s + x;
            float inflow = 0.0f;
            float outflow = 0.0f;
            outflow += flux_l_[i] + flux_r_[i] + flux_t_[i] + flux_b_[i];
            if (x > 0) inflow += flux_r_[i - 1];
            if (x < s - 1) inflow += flux_l_[i + 1];
            if (z > 0) inflow += flux_b_[i - s];
            if (z < s - 1) inflow += flux_t_[i + s];
            height_[i] += dt * (inflow - outflow) / cell_area;
        }
    }

    // 3) 随机扰动（雨滴）
    if (rain_rate > 0.0f && rain_strength != 0.0f) {
        const int drops = static_cast<int>(rain_rate * dt * static_cast<float>(s * s) / 4096.0f);
        for (int d = 0; d < drops; ++d) {
            rain_seed_ = rain_seed_ * 1664525u + 1013904223u;
            const int x = static_cast<int>((rain_seed_ >> 8) % static_cast<uint32_t>(s));
            rain_seed_ = rain_seed_ * 1664525u + 1013904223u;
            const int z = static_cast<int>((rain_seed_ >> 8) % static_cast<uint32_t>(s));
            height_[z * s + x] += rain_strength;
        }
    }

    // 4) NaN 防护：一旦数值炸了整个场重置（比持续输出 NaN 画面好恢复）
    if (!std::isfinite(height_[0]) || !std::isfinite(height_[height_.size() / 2])) {
        reset_to_rest();
        return;
    }
    gpu_dirty_ = true;
}

math::Vector3f FluidSurface3D::compute_normal(int x, int z) const {
    const int s = stride();
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);
    const auto at = [&](int xx, int zz) {
        xx = std::clamp(xx, 0, s - 1);
        zz = std::clamp(zz, 0, s - 1);
        return height_[zz * s + xx];
    };
    const float hl = at(x - 1, z);
    const float hr = at(x + 1, z);
    const float hd = at(x, z - 1);
    const float hu = at(x, z + 1);
    math::Vector3f normal(hl - hr, 2.0f * dx, hd - hu);
    return normal.normalized();
}

assets::MeshData FluidSurface3D::build_mesh_data() const {
    assets::MeshData data;
    const int s = stride();
    const float half_w = width * 0.5f;
    const float half_d = depth * 0.5f;
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);

    data.vertices.reserve(s * s);
    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            assets::MeshVertex v;
            v.position = math::Vector3f(-half_w + static_cast<float>(x) * dx,
                                        height_[z * s + x],
                                        -half_d + static_cast<float>(z) * dz);
            v.normal = compute_normal(x, z);
            v.tangent = math::Vector3f::right();
            v.uv = math::Vector2f(static_cast<float>(x) / static_cast<float>(resolution),
                                  static_cast<float>(z) / static_cast<float>(resolution));
            v.color = math::Vector3f::one();
            data.vertices.push_back(v);
        }
    }

    data.indices.reserve(resolution * resolution * 6);
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const uint32_t i0 = z * s + x;
            const uint32_t i1 = z * s + (x + 1);
            const uint32_t i2 = (z + 1) * s + x;
            const uint32_t i3 = (z + 1) * s + (x + 1);
            data.indices.push_back(i0);
            data.indices.push_back(i2);
            data.indices.push_back(i1);
            data.indices.push_back(i1);
            data.indices.push_back(i2);
            data.indices.push_back(i3);
        }
    }

    data.name = "FluidSurfaceMesh";
    return data;
}

void FluidSurface3D::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    queued_generation_.store(0, std::memory_order_release);
}

void FluidSurface3D::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                                  float& radius) const {
    // 水面在局部 Y ∈ [0, water_level + 扰动余量]，按静水位 + 50% 余量估
    const float top = water_level * 1.5f;
    center = world.transform_point(math::Vector3f(0.0f, top * 0.5f, 0.0f));
    const float half_w = width * 0.5f;
    const float half_d = depth * 0.5f;
    const float local_radius =
        std::sqrt(half_w * half_w + half_d * half_d + top * top * 0.25f);
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = local_radius * std::max(sx, std::max(sy, sz));
}

void FluidSurface3D::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && material_) material_->destroy_gpu(ctx_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        queued_generation_.store(0, std::memory_order_release);
        material_uploaded_.store(false, std::memory_order_release);
        ctx_ = ctx;
        gpu_dirty_ = true;
    }

    ensure_fields();

    // 网格重建：模拟推进过（gpu_dirty_）或尺寸/分辨率/水位变化。
    // 暂停且无编辑时每帧只做标量比对，不构建 MeshData、不投递命令。
    const bool params_changed = gpu_dirty_ || last_width_ != width || last_depth_ != depth ||
                                last_resolution_ != resolution ||
                                last_water_level_ != water_level;
    if (params_changed) {
        auto mesh = std::make_shared<assets::MeshData>(build_mesh_data());
        if (mesh->empty()) return;

        geometry_ = std::move(mesh);
        ++geometry_generation_;
        last_width_ = width;
        last_depth_ = depth;
        last_resolution_ = resolution;
        last_water_level_ = water_level;
        gpu_dirty_ = false;
    }
    if (!geometry_ || geometry_->empty()) return;

    const uint64_t generation = geometry_generation_;
    const bool need_geometry = generation != queued_generation_.load(std::memory_order_acquire);
    const bool need_material =
        material_ && !material_uploaded_.load(std::memory_order_acquire);
    if (!need_geometry && !need_material) return;

    auto geometry = geometry_;
    if (need_geometry) {
        queued_generation_.store(generation, std::memory_order_release);
    }
    ctx->push_command([this, ctx, geometry, need_geometry,
                       token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (need_geometry) {
            if (!gpu_mesh_handle_.is_valid()) {
                gpu_mesh_handle_ = ctx->create_mesh();
                render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
                if (!mesh) {
                    gpu_mesh_handle_ = render::RHIMeshHandle{};
                    return;
                }
                // 顶点布局：MeshVertex（position/normal/tangent/uv/color，stride 56），
                // 与 MeshRenderer/Terrain 一致（location 0..4）。
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
                // 顶点数恒定（分辨率不变时），原地重传顶点数据
                mesh->upload_vertices(
                    geometry->vertices.data(),
                    static_cast<uint32_t>(geometry->vertices.size() *
                                          sizeof(assets::MeshVertex)),
                    static_cast<uint32_t>(geometry->vertices.size()));
                mesh->upload_indices(
                    geometry->indices.data(),
                    static_cast<uint32_t>(geometry->indices.size() * sizeof(uint32_t)),
                    static_cast<uint32_t>(geometry->indices.size()));
            }
        }

        if (material_ && !material_uploaded_.exchange(true, std::memory_order_acq_rel)) {
            material_->upload_to_gpu(ctx);
        }
    });
}

} // namespace gryce_engine::components
