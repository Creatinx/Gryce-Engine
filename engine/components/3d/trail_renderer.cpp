#include "components/3d/trail_renderer.h"

#include <algorithm>

#include "render/mesh.h"
#include "scene/entity.h"

namespace gryce_engine::components {

namespace {

// 与 LineRenderer3D 相同的静态角标几何：两个三角形拼成一段四边形。
constexpr float k_ribbon_corners[6][2] = {
    {0.0f, -1.0f}, {1.0f, -1.0f}, {1.0f,  1.0f},
    {0.0f, -1.0f}, {1.0f,  1.0f}, {0.0f,  1.0f}
};

} // namespace

TrailRenderer::~TrailRenderer() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

void TrailRenderer::on_destroy() {
    alive_token_->store(false, std::memory_order_release);
}

void TrailRenderer::serialize(nlohmann::json& out) const {
    out["lifetime"] = lifetime;
    out["min_vertex_distance"] = min_vertex_distance;
    out["width"] = width;
    out["color"] = { color.r, color.g, color.b, color.a };
    out["autodestruct"] = autodestruct;
}

void TrailRenderer::deserialize(const nlohmann::json& in) {
    lifetime = in.value("lifetime", 0.3f);
    min_vertex_distance = in.value("min_vertex_distance", 0.02f);
    width = in.value("width", 0.1f);
    auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
    if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    autodestruct = in.value("autodestruct", false);
}

void TrailRenderer::on_update(float dt) {
    if (!owner_) return;
    elapsed_ += dt;

    const math::Vector3f now =
        owner_->world_transform().transform_point(math::Vector3f::zero());

    const float min_dist = std::max(min_vertex_distance, 0.0f);
    if (points_.empty()) {
        points_.push_back({now, elapsed_});
    } else {
        const math::Vector3f last = points_.back().position;
        const math::Vector3f d = now - last;
        if (d.length_sq() >= min_dist * min_dist) {
            points_.push_back({now, elapsed_});
        }
    }

    // 至少保留 2 个点，否则没有可画的段。
    const float cutoff = elapsed_ - std::max(lifetime, 0.0f);
    size_t drop = 0;
    while (points_.size() - drop > 2 && points_[drop].time < cutoff) ++drop;
    // 容量兜底同样从头部丢（最老的点）
    const size_t overflow = points_.size() - drop > k_max_points ? points_.size() - drop - k_max_points : 0;
    drop += overflow;
    if (drop > 0) {
        points_.erase(points_.begin(), points_.begin() + static_cast<ptrdiff_t>(drop));
    }
}

void TrailRenderer::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    gpu_ready_.store(false, std::memory_order_release);
}

void TrailRenderer::build_segments() {
    const size_t n = points_.size();
    if (n < 2) {
        segment_count_ = 0;
        return;
    }

    const uint32_t seg_count = static_cast<uint32_t>(n - 1);

    if (!scratch_[scratch_index_]) {
        scratch_[scratch_index_] = std::make_shared<std::vector<LineRenderer3D::SegmentInstance>>();
    }
    auto& buffer = *scratch_[scratch_index_];
    if (buffer.size() < seg_count) {
        buffer.resize(seg_count);
    }

    const float inv_life = lifetime > 1e-5f ? 1.0f / lifetime : 0.0f;
    for (uint32_t i = 0; i < seg_count; ++i) {
        const TrailPoint& a = points_[i];
        const TrailPoint& b = points_[i + 1];
        LineRenderer3D::SegmentInstance& inst = buffer[i];
        inst.sx = a.position.x;
        inst.sy = a.position.y;
        inst.sz = a.position.z;
        inst.ex = b.position.x;
        inst.ey = b.position.y;
        inst.ez = b.position.z;
        inst.width = width;
        // 越靠近尾端越透明：线性淡出，避免尾端几何被淘汰时"跳一下"。
        const float age = elapsed_ - b.time;
        const float fade = std::clamp(1.0f - age * inv_life, 0.0f, 1.0f);
        inst.r = color.r;
        inst.g = color.g;
        inst.b = color.b;
        inst.a = color.a * fade;
    }

    segment_count_ = seg_count;
}

void TrailRenderer::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        gpu_ready_.store(false, std::memory_order_release);
        ctx_ = ctx;
    }

    build_segments();
    if (segment_count_ == 0) return;

    auto buffer = scratch_[scratch_index_];
    scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;

    const uint32_t count = segment_count_;
    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(LineRenderer3D::SegmentInstance));

    ctx->push_command([this, ctx, buffer, count, bytes, token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 静态角标几何：6 顶点，只在创建时上传一次。
            render::VertexLayout corner_layout;
            corner_layout.stride = sizeof(float) * 2;
            corner_layout.attributes = {
                {0, render::VertexType::Float2, false, 0}
            };
            mesh->set_layout(corner_layout);
            mesh->upload_vertices(k_ribbon_corners, static_cast<uint32_t>(sizeof(k_ribbon_corners)), 6);

            // 实例流：与 line3d 着色器的 attribute location 1..4 一一对应。
            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(LineRenderer3D::SegmentInstance);
            instance_layout.attributes = {
                {1, render::VertexType::Float3, false, 0},
                {2, render::VertexType::Float3, false, 12},
                {3, render::VertexType::Float,  false, 24},
                {4, render::VertexType::Float4, false, 28}
            };
            mesh->set_instance_layout(instance_layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh) {
            mesh->upload_instances(buffer->data(), bytes, count);
        }
    });
}

} // namespace gryce_engine::components