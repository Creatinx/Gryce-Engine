#include "components/3d/particle_system_3d.h"

#include <algorithm>
#include <cmath>

#include "scene/entity.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_deg2rad = k_pi / 180.0f;

// 无 texture_path 时的内置回退贴图：程序化径向渐变（中心实、边缘柔、四角透明）。
// 不用 1x1 白色是因为那会让每颗粒子渲染成硬边方块；带衰减的贴图开箱即得柔和的
// 圆形粒子，同时保持"贴图 × 顶点色"单一路径（不需要 uUseTexture 分支，
// Vulkan 的 per-object UBO 没有可用的 int 字段位）。
render::RHITextureHandle create_default_particle_texture(render::RenderContext* ctx) {
    if (!ctx) return render::RHITextureHandle{};

    constexpr int k_size = 64;
    std::vector<unsigned char> rgba(static_cast<size_t>(k_size) * k_size * 4);
    const float half = static_cast<float>(k_size) * 0.5f;
    for (int y = 0; y < k_size; ++y) {
        for (int x = 0; x < k_size; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f - half) / half;
            const float dy = (static_cast<float>(y) + 0.5f - half) / half;
            const float r = std::sqrt(dx * dx + dy * dy);
            // 二次衰减：r=0 时 1，r>=1 时 0，边界连续
            const float falloff = std::clamp(1.0f - r, 0.0f, 1.0f);
            const float a = falloff * falloff;
            unsigned char* px = rgba.data() + (static_cast<size_t>(y) * k_size + x) * 4;
            px[0] = 255;
            px[1] = 255;
            px[2] = 255;
            px[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    }

    render::RHITextureHandle tex = ctx->create_texture();
    render::ITexture* tex_ptr = ctx->texture(tex);
    if (!tex.is_valid() || !tex_ptr) return render::RHITextureHandle{};
    if (!tex_ptr->upload_data(rgba.data(), k_size, k_size, 4)) {
        ctx->destroy_texture(tex);
        return render::RHITextureHandle{};
    }
    return tex;
}

render::Color lerp_color(const render::Color& a, const render::Color& b, float t) {
    return render::Color(a.r + (b.r - a.r) * t,
                         a.g + (b.g - a.g) * t,
                         a.b + (b.b - a.b) * t,
                         a.a + (b.a - a.a) * t);
}

// 构造与 dir 正交的两个基向量（dir 需已归一化）
void orthonormal_basis(const math::Vector3f& dir, math::Vector3f& out_t, math::Vector3f& out_b) {
    const math::Vector3f ref = (std::abs(dir.y) < 0.99f) ? math::Vector3f(0.0f, 1.0f, 0.0f)
                                                         : math::Vector3f(1.0f, 0.0f, 0.0f);
    out_t = dir.cross(ref).normalized();
    out_b = dir.cross(out_t).normalized();
}

} // namespace

std::mt19937& ParticleSystem3D::rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

float ParticleSystem3D::random_float(float min, float max) {
    if (min >= max) return min;
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng());
}

ParticleSystem3D::ParticleSystem3D() {
    for (auto& slot : scratch_) {
        slot = std::make_shared<std::vector<ParticleInstance>>();
    }
}

ParticleSystem3D::~ParticleSystem3D() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
    if (texture_handle_.is_valid()) {
        ctx_->destroy_texture(texture_handle_);
        texture_handle_ = render::RHITextureHandle{};
    }
}

void ParticleSystem3D::invalidate_gpu() {
    if (ctx_) {
        if (gpu_mesh_handle_.is_valid()) {
            ctx_->destroy_mesh(gpu_mesh_handle_);
        }
        if (texture_handle_.is_valid()) {
            ctx_->destroy_texture(texture_handle_);
        }
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    texture_handle_ = render::RHITextureHandle{};
    gpu_ready_.store(false, std::memory_order_release);
}

void ParticleSystem3D::on_destroy() {
    alive_token_->store(false, std::memory_order_release);
}

int ParticleSystem3D::active_count() const {
    // 容器只保存存活粒子，大小即数量
    return static_cast<int>(particles_.size());
}

void ParticleSystem3D::clear() {
    particles_.clear();
    emission_accumulator_ = 0.0f;
}

void ParticleSystem3D::burst() {
    const int count = std::max(1, max_particles / 8);
    const math::Matrix4f model = owner_ ? owner_->world_transform() : math::Matrix4f::identity();
    emit_internal(count, model);
}

void ParticleSystem3D::emit(int count) {
    const math::Matrix4f model = owner_ ? owner_->world_transform() : math::Matrix4f::identity();
    emit_internal(count, model);
}

void ParticleSystem3D::emit_internal(int count, const math::Matrix4f& model) {
    // 稠密容器：剩余容量 = 上限 - 当前存活数，O(1)
    const int slots = std::max(0, max_particles) - static_cast<int>(particles_.size());
    if (slots <= 0) return;
    count = std::min(count, slots);
    for (int i = 0; i < count; ++i) {
        spawn_particle(model);
    }
}

void ParticleSystem3D::spawn_particle(const math::Matrix4f& model) {
    Particle p;

    // 发射形状决定局部空间的起始偏移
    math::Vector3f local_pos = emission_offset;
    switch (static_cast<Shape>(shape)) {
        case Shape::Sphere: {
            // 单位球内均匀采样（按立方根缩放半径，避免向球心聚集）
            const float theta = random_float(0.0f, 2.0f * k_pi);
            const float z = random_float(-1.0f, 1.0f);
            const float r = shape_radius * std::cbrt(random_float(0.0f, 1.0f));
            const float xy = std::sqrt(std::max(0.0f, 1.0f - z * z));
            local_pos = local_pos + math::Vector3f(std::cos(theta) * xy, std::sin(theta) * xy, z) * r;
            break;
        }
        case Shape::Box:
            local_pos = local_pos + math::Vector3f(random_float(-shape_extents.x, shape_extents.x),
                                                   random_float(-shape_extents.y, shape_extents.y),
                                                   random_float(-shape_extents.z, shape_extents.z));
            break;
        case Shape::Point:
        default:
            break;
    }

    // 发射方向：以 direction 为轴、spread_angle 为半角的圆锥内随机
    math::Vector3f dir = direction;
    if (dir.length_sq() < 1e-8f) dir = math::Vector3f(0.0f, 1.0f, 0.0f);
    dir = dir.normalized();
    if (spread_angle > 0.0f) {
        math::Vector3f t, b;
        orthonormal_basis(dir, t, b);
        const float cone = random_float(0.0f, spread_angle) * k_deg2rad;
        const float phi = random_float(0.0f, 2.0f * k_pi);
        dir = (dir * std::cos(cone) + (t * std::cos(phi) + b * std::sin(phi)) * std::sin(cone)).normalized();
    }

    const float speed = random_float(speed_min, speed_max);
    math::Vector3f local_vel = dir * speed;

    // 世界空间模拟：出生瞬间就把位置/速度转到世界空间，之后脱离发射器
    if (simulate_in_world_space) {
        p.position = model.transform_point(local_pos);
        p.velocity = model.transform_vector(local_vel);
    } else {
        p.position = local_pos;
        p.velocity = local_vel;
    }

    p.age = 0.0f;
    p.lifetime = std::max(0.0001f, random_float(lifetime_min, lifetime_max));
    p.start_size = start_size;
    p.end_size = end_size;
    p.start_color = start_color;
    p.end_color = end_color;
    p.rotation = random_float(rotation_min, rotation_max) * k_deg2rad;
    p.angular_velocity = random_float(angular_velocity_min, angular_velocity_max) * k_deg2rad;

    // 容量由 emit_internal 保证；容器稠密，直接追加
    if (static_cast<int>(particles_.size()) < max_particles) {
        particles_.push_back(p);
    }
}

void ParticleSystem3D::on_update(float dt) {
    if (!enabled || dt <= 0.0f) return;

    const math::Matrix4f model = owner_ ? owner_->world_transform() : math::Matrix4f::identity();
    elapsed_ += dt;

    // loop=false：发射时长按最长寿命估计，一轮粒子自然消亡后不再补充
    if (!loop && elapsed_ > lifetime_max) {
        emitting_ = false;
    }

    if (play_on_awake && emitting_ && emission_rate > 0.0f) {
        emission_accumulator_ += emission_rate * dt;
        const int to_emit = static_cast<int>(emission_accumulator_);
        if (to_emit > 0) {
            emission_accumulator_ -= static_cast<float>(to_emit);
            emit_internal(to_emit, model);
        }
    }

    // swap-remove：死亡粒子用尾部元素填补并弹出。容器始终稠密，
    // 单帧成本是 O(存活数)，且不会像旧的"线性找空槽"那样退化成 O(n²)。
    for (size_t i = 0; i < particles_.size(); ) {
        Particle& p = particles_[i];
        p.age += dt;
        if (p.age >= p.lifetime) {
            particles_[i] = particles_.back();
            particles_.pop_back();
            continue;   // 换过来的元素尚未更新，原地重来
        }
        p.velocity = p.velocity + acceleration * dt;
        p.position = p.position + p.velocity * dt;
        p.rotation += p.angular_velocity * dt;
        ++i;
    }
}

void ParticleSystem3D::build_instances(const math::Matrix4f& model) {
    const uint32_t cap = static_cast<uint32_t>(std::max(1, max_particles));
    auto& buffer = *scratch_[scratch_index_];
    if (buffer.size() != cap) {
        buffer.assign(cap, ParticleInstance{});
    }

    // 局部空间模拟时粒子存的是发射器局部坐标，绘制前统一变换到世界空间；
    // 世界空间模拟时位置已是世界坐标，用单位矩阵避免二次变换。
    const math::Matrix4f xform = simulate_in_world_space ? math::Matrix4f::identity() : model;

    uint32_t w = 0;
    for (const auto& p : particles_) {
        if (w >= cap) break;

        const float t = p.lifetime > 0.0f ? std::min(1.0f, p.age / p.lifetime) : 0.0f;
        const float curve_t = (std::abs(size_curve - 1.0f) < 1e-4f) ? t : std::pow(t, size_curve);
        const render::Color c = lerp_color(p.start_color, p.end_color, t);
        const math::Vector3f world = xform.transform_point(p.position);

        ParticleInstance& inst = buffer[w];
        inst.px = world.x;
        inst.py = world.y;
        inst.pz = world.z;
        inst.size = p.start_size + (p.end_size - p.start_size) * curve_t;
        inst.rotation = p.rotation;
        inst.r = c.r;
        inst.g = c.g;
        inst.b = c.b;
        inst.a = c.a;
        ++w;
    }

    used_instances_ = w;
}

void ParticleSystem3D::prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model) {
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

    // 贴图路径变化：销毁后下一段命令里重建（含贴图）
    if (texture_path != texture_path_loaded_) {
        invalidate_gpu();
        texture_path_loaded_ = texture_path;
    }

    build_instances(model);
    if (used_instances_ == 0) return;

    const uint32_t cap = static_cast<uint32_t>(std::max(1, max_particles));
    // 首帧按满容量分配 GPU 缓冲（此时只画 used_instances_ 个实例），
    // 之后每帧只传存活数量，避免 Vulkan 侧随粒子数起伏反复重建缓冲。
    const bool size_buffer = !gpu_ready_.load(std::memory_order_acquire);
    const uint32_t upload_bytes =
        (size_buffer ? cap : used_instances_) * static_cast<uint32_t>(sizeof(ParticleInstance));
    const uint32_t used = used_instances_;

    auto buffer = scratch_[scratch_index_];
    scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;

    const std::string tex_path = texture_path;
    ctx->push_command([this, ctx, buffer, upload_bytes, used, tex_path,
                       token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 广告牌原型几何：6 顶点的单位四边形，只在创建时上传一次。
            // 每颗粒子只贡献一条实例数据，四边形角由顶点流提供。
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

            // 实例流：center(3)+size(1)+rotation(1)+color(4)，与 vulkan_particle 的
            // binding 1 描述一致。
            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(ParticleInstance);
            instance_layout.attributes = {
                {1, render::VertexType::Float3, false, 0},
                {2, render::VertexType::Float,  false, 12},
                {3, render::VertexType::Float,  false, 16},
                {4, render::VertexType::Float4, false, 20}
            };
            mesh->set_instance_layout(instance_layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh && upload_bytes <= buffer->size() * sizeof(ParticleInstance)) {
            mesh->upload_instances(buffer->data(), upload_bytes, used);
        }

        if (!texture_handle_.is_valid()) {
            if (!tex_path.empty()) {
                texture_handle_ = render::load_texture_from_path(ctx, tex_path);
                if (!texture_handle_.is_valid()) {
                    GLOG_WARN("ParticleSystem3D: texture '{}' unavailable, using built-in radial fallback", tex_path);
                }
            }
            if (!texture_handle_.is_valid()) {
                texture_handle_ = create_default_particle_texture(ctx);
            }
        }

        gpu_ready_.store(true, std::memory_order_release);
    });
}

void ParticleSystem3D::serialize(nlohmann::json& out) const {
    out["texture_path"] = texture_path;
    out["loop"] = loop;
    out["play_on_awake"] = play_on_awake;
    out["max_particles"] = max_particles;
    out["emission_rate"] = emission_rate;
    out["lifetime_min"] = lifetime_min;
    out["lifetime_max"] = lifetime_max;
    out["speed_min"] = speed_min;
    out["speed_max"] = speed_max;
    out["start_size"] = start_size;
    out["end_size"] = end_size;
    out["start_color"] = { start_color.r, start_color.g, start_color.b, start_color.a };
    out["end_color"] = { end_color.r, end_color.g, end_color.b, end_color.a };
    out["additive"] = additive;
    out["emission_offset"] = { emission_offset.x, emission_offset.y, emission_offset.z };
    out["acceleration"] = { acceleration.x, acceleration.y, acceleration.z };
    out["direction"] = { direction.x, direction.y, direction.z };
    out["spread_angle"] = spread_angle;
    out["shape"] = shape;
    out["shape_radius"] = shape_radius;
    out["shape_extents"] = { shape_extents.x, shape_extents.y, shape_extents.z };
    out["rotation_min"] = rotation_min;
    out["rotation_max"] = rotation_max;
    out["angular_velocity_min"] = angular_velocity_min;
    out["angular_velocity_max"] = angular_velocity_max;
    out["size_curve"] = size_curve;
    out["simulate_in_world_space"] = simulate_in_world_space;
    out["depth_test"] = depth_test;
}

void ParticleSystem3D::deserialize(const nlohmann::json& in) {
    texture_path = in.value("texture_path", "");
    loop = in.value("loop", true);
    play_on_awake = in.value("play_on_awake", true);
    max_particles = in.value("max_particles", 256);
    emission_rate = in.value("emission_rate", 10.0f);
    lifetime_min = in.value("lifetime_min", 1.0f);
    lifetime_max = in.value("lifetime_max", 2.0f);
    speed_min = in.value("speed_min", 1.0f);
    speed_max = in.value("speed_max", 3.0f);
    start_size = in.value("start_size", 0.2f);
    end_size = in.value("end_size", 0.05f);
    auto sc = in.value("start_color", std::vector<float>{1, 1, 1, 1});
    if (sc.size() >= 4) start_color = render::Color(sc[0], sc[1], sc[2], sc[3]);
    auto ec = in.value("end_color", std::vector<float>{1, 1, 1, 0});
    if (ec.size() >= 4) end_color = render::Color(ec[0], ec[1], ec[2], ec[3]);
    additive = in.value("additive", false);
    auto eo = in.value("emission_offset", std::vector<float>{0, 0, 0});
    if (eo.size() >= 3) emission_offset = math::Vector3f(eo[0], eo[1], eo[2]);
    auto acc = in.value("acceleration", std::vector<float>{0.0f, -9.8f, 0.0f});
    if (acc.size() >= 3) acceleration = math::Vector3f(acc[0], acc[1], acc[2]);
    auto dir = in.value("direction", std::vector<float>{0.0f, 1.0f, 0.0f});
    if (dir.size() >= 3) direction = math::Vector3f(dir[0], dir[1], dir[2]);
    spread_angle = in.value("spread_angle", 25.0f);
    shape = in.value("shape", 0);
    shape_radius = in.value("shape_radius", 0.1f);
    auto ext = in.value("shape_extents", std::vector<float>{0.1f, 0.1f, 0.1f});
    if (ext.size() >= 3) shape_extents = math::Vector3f(ext[0], ext[1], ext[2]);
    rotation_min = in.value("rotation_min", 0.0f);
    rotation_max = in.value("rotation_max", 0.0f);
    angular_velocity_min = in.value("angular_velocity_min", 0.0f);
    angular_velocity_max = in.value("angular_velocity_max", 0.0f);
    size_curve = in.value("size_curve", 1.0f);
    simulate_in_world_space = in.value("simulate_in_world_space", false);
    depth_test = in.value("depth_test", true);

    // 参数变化后 GPU 侧的顶点容量/贴图可能失效，交给下一帧 prepare_gpu 重建
    texture_path_loaded_.clear();
}

} // namespace gryce_engine::components