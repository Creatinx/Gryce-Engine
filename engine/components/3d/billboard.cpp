#include "components/3d/billboard.h"

#include <cmath>

#include "render/material.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// 静态原型几何：两个三角形拼成的单位四边形，corner.xy ∈ [-1,1]。
// 顶点着色器把它按广告牌尺寸缩放，再用相机基向量展开到世界空间，只上传一次。
constexpr float k_quad_corners[6][2] = {
    {-1.0f, -1.0f}, { 1.0f, -1.0f}, { 1.0f,  1.0f},
    {-1.0f, -1.0f}, { 1.0f,  1.0f}, {-1.0f,  1.0f}
};

// 无 texture_path 时的内置回退贴图：2x2 不透明纯白。
// 与 ParticleSystem3D 的径向渐变回退不同 —— 广告牌是矩形贴图（血条/公告/招牌），
// 白底矩形正是"没有贴图时的预期外观"；径向渐变会让它变成一团光晕。
render::RHITextureHandle create_default_billboard_texture(render::RenderContext* ctx) {
    if (!ctx) return render::RHITextureHandle{};

    constexpr int k_size = 2;
    constexpr unsigned char k_white[k_size * k_size * 4] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255
    };

    render::RHITextureHandle tex = ctx->create_texture();
    render::ITexture* tex_ptr = ctx->texture(tex);
    if (!tex.is_valid() || !tex_ptr) return render::RHITextureHandle{};
    if (!tex_ptr->upload_data(k_white, k_size, k_size, 4)) {
        ctx->destroy_texture(tex);
        return render::RHITextureHandle{};
    }
    return tex;
}

} // namespace

Billboard::~Billboard() {
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

void Billboard::serialize(nlohmann::json& out) const {
    out["texture_path"] = texture_path;
    out["lock_x_axis"] = lock_x_axis;
    out["size"] = { size.x, size.y };
    out["opacity"] = opacity;
    out["shaded"] = shaded;
}

void Billboard::deserialize(const nlohmann::json& in) {
    texture_path = in.value("texture_path", "");
    lock_x_axis = in.value("lock_x_axis", true);
    auto s = in.value("size", std::vector<float>{1, 1});
    if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
    opacity = in.value("opacity", 1.0f);
    shaded = in.value("shaded", false);

    // 贴图路径可能被整体替换，交给下一帧 prepare_gpu 重建
    texture_path_loaded_.clear();
}

void Billboard::invalidate_gpu() {
    if (ctx_) {
        if (gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (texture_handle_.is_valid()) ctx_->destroy_texture(texture_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    texture_handle_ = render::RHITextureHandle{};
}

void Billboard::prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && texture_handle_.is_valid()) ctx_->destroy_texture(texture_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        texture_handle_ = render::RHITextureHandle{};
        ctx_ = ctx;
    }

    // 贴图路径变化：销毁后下一段命令里重建（含贴图）
    if (texture_path != texture_path_loaded_) {
        invalidate_gpu();
        texture_path_loaded_ = texture_path;
    }

    // 全部实例统一在世界空间生成，模型矩阵只用来取中心点（广告牌只有位置有意义）
    const math::Vector3f center = model.transform_point(math::Vector3f(0.0f, 0.0f, 0.0f));

    if (!scratch_[scratch_index_]) {
        scratch_[scratch_index_] = std::make_shared<std::vector<BillboardInstance>>();
    }
    auto& buffer = *scratch_[scratch_index_];
    if (buffer.empty()) buffer.resize(1);

    BillboardInstance& inst = buffer[0];
    inst.px = center.x;
    inst.py = center.y;
    inst.pz = center.z;
    // size 为负时用绝对值，避免两面同向绕序导致的退化四边形
    inst.sx = std::abs(size.x);
    inst.sy = std::abs(size.y);
    inst.opacity = opacity;
    inst.lock_x = lock_x_axis ? 1.0f : 0.0f;
    inst.shaded = shaded ? 1.0f : 0.0f;
    instance_count_ = 1;

    auto buffer_ptr = scratch_[scratch_index_];
    scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;

    const uint32_t count = instance_count_;
    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(BillboardInstance));
    const std::string tex_path = texture_path;

    ctx->push_command([this, ctx, buffer_ptr, count, bytes, tex_path,
                       token = alive_token_](render::IRenderBackend*) {
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
            mesh->upload_vertices(k_quad_corners, static_cast<uint32_t>(sizeof(k_quad_corners)), 6);

            // 实例流：center(3)+size(2)+opacity(1)+flags(2)，与 billboard 着色器的
            // attribute location 1..4 一一对应。
            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(BillboardInstance);
            instance_layout.attributes = {
                {1, render::VertexType::Float3, false, 0},
                {2, render::VertexType::Float2, false, 12},
                {3, render::VertexType::Float,  false, 20},
                {4, render::VertexType::Float2, false, 24}
            };
            mesh->set_instance_layout(instance_layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh) {
            mesh->upload_instances(buffer_ptr->data(), bytes, count);
        }

        if (!texture_handle_.is_valid()) {
            if (!tex_path.empty()) {
                texture_handle_ = render::load_texture_from_path(ctx, tex_path);
                if (!texture_handle_.is_valid()) {
                    GLOG_WARN("Billboard: texture '{}' unavailable, using built-in white fallback", tex_path);
                }
            }
            if (!texture_handle_.is_valid()) {
                texture_handle_ = create_default_billboard_texture(ctx);
            }
        }
    });
}

} // namespace gryce_engine::components