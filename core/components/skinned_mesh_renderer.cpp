#include "skinned_mesh_renderer.h"

#include "render/skinned_vertex.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

SkinnedMeshRenderer::SkinnedMeshRenderer() {
    material = std::make_unique<render::Material>();
}

SkinnedMeshRenderer::SkinnedMeshRenderer(const std::string& path) : model_path(path) {
    material = std::make_unique<render::Material>();
}

SkinnedMeshRenderer::~SkinnedMeshRenderer() {
    // 使延迟执行的异步上传命令失效，避免渲染线程访问已析构的 this
    alive_token_->store(false, std::memory_order_release);
    if (material && ctx_) {
        material->destroy_gpu(ctx_);
    }
    if (gpu_mesh_handle_.is_valid() && ctx_) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

render::Material* SkinnedMeshRenderer::ensure_material() {
    if (!material) {
        material = std::make_unique<render::Material>();
    }
    return material.get();
}

void SkinnedMeshRenderer::serialize(nlohmann::json& out) const {
    out["model_path"] = model_path;
    out["clip_name"] = clip_name;
    out["playing"] = playing;
    out["loop"] = loop;
    out["speed"] = speed;
    if (material) {
        nlohmann::json mat_json;
        material->serialize(mat_json);
        out["material"] = std::move(mat_json);
    }
}

void SkinnedMeshRenderer::deserialize(const nlohmann::json& in) {
    model_path = in.value("model_path", "");
    clip_name = in.value("clip_name", "");
    playing = in.value("playing", true);
    loop = in.value("loop", true);
    speed = in.value("speed", 1.0f);
    time = 0.0f;
    ensure_material();
    auto it = in.find("material");
    if (it != in.end()) {
        material->deserialize(*it);
    }
}

void SkinnedMeshRenderer::set_model(std::shared_ptr<assets::SkinnedModelData> model) {
    model_ = std::move(model);
}

const animation::AnimationClip* SkinnedMeshRenderer::resolve_clip() const {
    if (!model_ || model_->animations.empty()) return nullptr;
    if (!clip_name.empty()) {
        for (const auto& clip : model_->animations) {
            if (clip.name == clip_name) return &clip;
        }
        GLOG_WARN("SkinnedMeshRenderer: clip '{}' not found in '{}', fallback to first",
                  clip_name, model_path);
    }
    return &model_->animations.front();
}

void SkinnedMeshRenderer::set_palette(std::shared_ptr<const std::vector<math::Matrix4f>> palette) {
    palette_ = std::move(palette);
}

render::IMesh* SkinnedMeshRenderer::upload_to_gpu(render::RenderContext* ctx, bool allow_while_running) {
    if (!ctx || !model_ || model_->meshes.empty()) return nullptr;
    if (uploaded_.load(std::memory_order_acquire) && ctx_ == ctx) return gpu_mesh();

    // 渲染线程已接管 GL context 后主线程不能再做 GL 上传；
    // allow_while_running=true 表示调用方已确保在渲染线程/命令队列中执行。
    if (!allow_while_running && ctx->is_running()) {
        return nullptr;
    }

    // 本轮只上传第一个带 skin 的 mesh（多 mesh 模型为已知遗留项）
    const assets::SkinnedMeshData* mesh_data = nullptr;
    for (const auto& m : model_->meshes) {
        if (m.has_skin()) { mesh_data = &m; break; }
    }
    if (!mesh_data) mesh_data = &model_->meshes.front();
    if (mesh_data->empty()) return nullptr;

    ctx_ = ctx;

    std::vector<render::SkinnedVertexGPU> verts = render::build_skinned_vertices(*mesh_data);

    render::RHIMeshHandle mesh = ctx->create_mesh();
    render::IMesh* mesh_ptr = ctx->mesh(mesh);
    if (!mesh.is_valid() || !mesh_ptr) return nullptr;

    mesh_ptr->upload_vertices(verts.data(),
                              static_cast<uint32_t>(verts.size() * sizeof(render::SkinnedVertexGPU)),
                              static_cast<uint32_t>(verts.size()));
    mesh_ptr->upload_indices(mesh_data->indices.data(),
                             static_cast<uint32_t>(mesh_data->indices.size() * sizeof(uint32_t)),
                             static_cast<uint32_t>(mesh_data->indices.size()));
    mesh_ptr->set_layout(render::skinned_vertex_layout());

    // 模型自带材质合并（与 MeshRenderer 相同策略：组件显式设置优先）
    if (material && mesh_data->material.valid) {
        const assets::MeshMaterialData& mm = mesh_data->material;
        const math::Vector3f one = math::Vector3f::one();
        const math::Vector3f zero = math::Vector3f::zero();
        if (material->albedo_color == one) material->albedo_color = mm.albedo_color;
        if (material->emissive_color == zero) material->emissive_color = mm.emissive_color;
        if (material->opacity == 1.0f && mm.opacity < 1.0f) {
            material->opacity = mm.opacity;
            material->blend_mode = render::Material::BlendMode::Blend;
        }
        if (material->roughness == 0.5f) material->roughness = mm.roughness;
        if (material->metallic == 0.0f) material->metallic = mm.metallic;
        if (material->albedo_map_path.empty()) material->albedo_map_path = mm.albedo_map;
        if (material->normal_map_path.empty()) material->normal_map_path = mm.normal_map;
        if (material->emissive_map_path.empty()) material->emissive_map_path = mm.emissive_map;
        if (material->roughness_map_path.empty()) material->roughness_map_path = mm.roughness_map;
        if (material->metallic_map_path.empty()) material->metallic_map_path = mm.metallic_map;
        if (material->ao_map_path.empty()) material->ao_map_path = mm.ao_map;
    }

    if (material) {
        material->upload_to_gpu(ctx);
    }

    gpu_mesh_handle_ = mesh;
    uploaded_.store(true, std::memory_order_release);

    GLOG_INFO("SkinnedMeshRenderer: uploaded '{}' to GPU ({} vertices, {} indices, skin={})",
              model_path, verts.size(), mesh_data->indices.size(), mesh_data->has_skin());
    return mesh_ptr;
}

} // namespace gryce_engine::components
