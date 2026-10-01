#include "components/3d/volumetric_light.h"

#include <algorithm>
#include <cmath>

#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

// 光柱半张角（度）：远端半径 = range * tan(25°) ≈ 0.466*range
constexpr float k_half_angle_deg = 25.0f;
// 角向落点：视线与光轴夹角在 12° 内几乎全亮，到 70° 衰减到 0
constexpr float k_cos_inner = 0.9781476f;  // cos(12°)
constexpr float k_cos_outer = 0.3420201f;  // cos(70°)
// 锥面圆周分段数（与 steps 无关，保证锥面足够圆）
constexpr int k_radial_segments = 20;

// 确定性哈希噪声 [-1,1]：jitter 用它给各轴向环的半径加确定性扰动
// （不用随机数，保证同一参数每次生成的几何完全一致，便于缓存/复现）。
float hash_noise(int i) {
    int n = i * 374761393 + 668265263;
    n = (n ^ (n >> 13)) * 1274126177;
    n = n ^ (n >> 16);
    return static_cast<float>(n & 0x7fffffff) / static_cast<float>(0x7fffffff) * 2.0f - 1.0f;
}

} // namespace

VolumetricLight::~VolumetricLight() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

void VolumetricLight::serialize(nlohmann::json& out) const {
    out["intensity"] = intensity;
    out["range"] = range;
    out["color"] = { color.r, color.g, color.b, color.a };
    out["steps"] = steps;
    out["jitter"] = jitter;
}

void VolumetricLight::deserialize(const nlohmann::json& in) {
    intensity = in.value("intensity", 1.0f);
    range = in.value("range", 10.0f);
    auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
    if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    steps = in.value("steps", 16);
    jitter = in.value("jitter", 0.1f);

    // 参数可能被整体替换，交给下一帧 prepare_gpu 重建
    gpu_dirty_ = true;
}

void VolumetricLight::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    // 代次归零：下一帧 prepare_gpu 会重新投递一次上传命令
    queued_generation_.store(0, std::memory_order_release);
    gpu_dirty_ = true;
}

void VolumetricLight::build_geometry() {
    const int rings = std::clamp(steps, 2, 64);
    const float range_len = std::max(range, 0.01f);
    const float r_far = range_len * std::tan(k_half_angle_deg * k_pi / 180.0f);
    const float r_apex = range_len * 0.02f;
    const float jitter_amt = std::clamp(jitter, 0.0f, 1.0f);

    auto verts = std::make_shared<std::vector<ShaftVertex>>();
    verts->reserve(static_cast<size_t>(rings) * k_radial_segments);

    for (int i = 0; i < rings; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rings - 1);
        // 沿轴向线性张开的锥面半径，叠加确定性扰动打散环带
        float radius = r_apex + (r_far - r_apex) * t;
        radius *= 1.0f + jitter_amt * 0.25f * hash_noise(i);
        if (radius < 1e-4f) radius = 1e-4f;
        const float z = -range_len * t;

        for (int j = 0; j < k_radial_segments; ++j) {
            const float ang = 2.0f * k_pi * static_cast<float>(j) / static_cast<float>(k_radial_segments);
            ShaftVertex v;
            v.px = std::cos(ang) * radius;
            v.py = std::sin(ang) * radius;
            v.pz = z;
            v.t = t;
            v.cos_inner = k_cos_inner;
            v.cos_outer = k_cos_outer;
            v.range = range_len;
            // rgb 预先乘上 intensity（HDR 下参与 bloom），a 保留颜色不透明度
            v.r = color.r * intensity;
            v.g = color.g * intensity;
            v.b = color.b * intensity;
            v.a = color.a;
            verts->push_back(v);
        }
    }

    auto idx = std::make_shared<std::vector<uint32_t>>();
    idx->reserve(static_cast<size_t>(rings - 1) * k_radial_segments * 6);
    for (int i = 0; i < rings - 1; ++i) {
        for (int j = 0; j < k_radial_segments; ++j) {
            const uint32_t j1 = static_cast<uint32_t>((j + 1) % k_radial_segments);
            const uint32_t a = static_cast<uint32_t>(i * k_radial_segments) + static_cast<uint32_t>(j);
            const uint32_t b = static_cast<uint32_t>(i * k_radial_segments) + j1;
            const uint32_t c = static_cast<uint32_t>((i + 1) * k_radial_segments) + static_cast<uint32_t>(j);
            const uint32_t d = static_cast<uint32_t>((i + 1) * k_radial_segments) + j1;
            idx->push_back(a); idx->push_back(b); idx->push_back(c);
            idx->push_back(b); idx->push_back(d); idx->push_back(c);
        }
    }

    vertices_ = std::move(verts);
    indices_ = std::move(idx);
    vertex_count_ = static_cast<uint32_t>(vertices_->size());
}

void VolumetricLight::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                                   float& radius) const {
    const float range_len = std::max(range, 0.01f);
    const float r_far = range_len * std::tan(k_half_angle_deg * k_pi / 180.0f);
    // 局部锥体的中段中心：沿 -Z 走一半长度
    center = world.transform_point(math::Vector3f(0.0f, 0.0f, -range_len * 0.5f));
    const float local_radius = 0.5f * std::sqrt(range_len * range_len + 4.0f * r_far * r_far);

    // 世界变换的最大轴缩放（旋转不改变包围球半径）
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = local_radius * std::max(sx, std::max(sy, sz));
}

void VolumetricLight::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        queued_generation_.store(0, std::memory_order_release);
        ctx_ = ctx;
        gpu_dirty_ = true;
    }

    // 仅参数变化时重建几何；静止光柱每帧只做一次标量比对，无堆分配、无上传。
    const bool params_changed = gpu_dirty_ || last_range_ != range || last_steps_ != steps ||
                                last_jitter_ != jitter;
    if (params_changed) {
        build_geometry();
        ++geometry_generation_;
        last_range_ = range;
        last_steps_ = steps;
        last_jitter_ = jitter;
        gpu_dirty_ = false;
    }
    if (!vertices_ || vertices_->empty()) return;

    const uint64_t generation = geometry_generation_;
    const bool need_geometry = generation != queued_generation_.load(std::memory_order_acquire);
    if (!need_geometry) return;

    // 先登记代次再投递：命令在渲染线程执行前，本帧不会再重复投递同一份几何
    queued_generation_.store(generation, std::memory_order_release);
    auto vertices = vertices_;
    auto indices = indices_;

    ctx->push_command([this, ctx, vertices, indices, token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 顶点布局：position(3) + params(4) + color(4)，stride 44，与
            // voluelight 着色器的 attribute location 0..2 一一对应。
            render::VertexLayout layout;
            layout.stride = sizeof(ShaftVertex);
            layout.attributes = {
                {0, render::VertexType::Float3, false, 0},
                {1, render::VertexType::Float4, false, 12},
                {2, render::VertexType::Float4, false, 28}
            };
            mesh->set_layout(layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh) {
            mesh->upload_vertices(vertices->data(),
                                  static_cast<uint32_t>(vertices->size() * sizeof(ShaftVertex)),
                                  static_cast<uint32_t>(vertices->size()));
            mesh->upload_indices(indices->data(),
                                 static_cast<uint32_t>(indices->size() * sizeof(uint32_t)),
                                 static_cast<uint32_t>(indices->size()));
        }
    });
}

} // namespace gryce_engine::components