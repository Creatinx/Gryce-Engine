#include "components/3d/line_renderer_3d.h"

#include <algorithm>

#include "render/mesh.h"

namespace gryce_engine::components {

namespace {

// 静态角标几何：两个三角形拼成一段四边形。
//   corner.x: 0 = 起点，1 = 终点
//   corner.y: -1 / +1 = 线段两侧
// 顶点着色器用 mix(aStart, aEnd, corner.x) 与相机基向量算出世界坐标。
constexpr float k_ribbon_corners[6][2] = {
    {0.0f, -1.0f}, {1.0f, -1.0f}, {1.0f,  1.0f},
    {0.0f, -1.0f}, {1.0f,  1.0f}, {0.0f,  1.0f}
};

} // namespace

LineRenderer3D::~LineRenderer3D() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

void LineRenderer3D::serialize(nlohmann::json& out) const {
    out["loop"] = loop;
    out["width"] = width;
    out["color"] = { color.r, color.g, color.b, color.a };
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : points) arr.push_back({ p.x, p.y, p.z });
    out["points"] = std::move(arr);
}

void LineRenderer3D::deserialize(const nlohmann::json& in) {
    loop = in.value("loop", false);
    width = in.value("width", 0.05f);
    auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
    if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    points.clear();
    if (in.contains("points") && in["points"].is_array()) {
        for (const auto& item : in["points"]) {
            if (item.is_array() && item.size() >= 3) {
                points.emplace_back(item[0].get<float>(), item[1].get<float>(), item[2].get<float>());
            }
        }
    }
}

void LineRenderer3D::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    gpu_ready_.store(false, std::memory_order_release);
}

void LineRenderer3D::build_segments(const math::Matrix4f& model) {
    const size_t n = points.size();
    if (n < 2) {
        segment_count_ = 0;
        return;
    }

    const uint32_t seg_count = static_cast<uint32_t>(loop ? n : n - 1);

    if (!scratch_[scratch_index_]) {
        scratch_[scratch_index_] = std::make_shared<std::vector<SegmentInstance>>();
    }
    auto& buffer = *scratch_[scratch_index_];
    if (buffer.size() < seg_count) {
        buffer.resize(seg_count);
    }

    for (uint32_t i = 0; i < seg_count; ++i) {
        const math::Vector3f a = model.transform_point(points[i]);
        const math::Vector3f b = model.transform_point(points[(i + 1) % n]);
        SegmentInstance& inst = buffer[i];
        inst.sx = a.x;
        inst.sy = a.y;
        inst.sz = a.z;
        inst.ex = b.x;
        inst.ey = b.y;
        inst.ez = b.z;
        inst.width = width;
        inst.r = color.r;
        inst.g = color.g;
        inst.b = color.b;
        inst.a = color.a;
    }

    segment_count_ = seg_count;
}

void LineRenderer3D::prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        gpu_ready_.store(false, std::memory_order_release);
        ctx_ = ctx;
    }

    build_segments(model);
    if (segment_count_ == 0) return;

    auto buffer = scratch_[scratch_index_];
    scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;

    const uint32_t count = segment_count_;
    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(SegmentInstance));

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

            // 实例流：start(3)+end(3)+width(1)+color(4)，与 line3d 着色器的
            // attribute location 1..4 一一对应。
            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(SegmentInstance);
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