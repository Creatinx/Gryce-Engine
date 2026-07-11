#include "mesh_renderer.h"

#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

MeshRenderer::MeshRenderer() {
    material = std::make_unique<render::Material>();
}

MeshRenderer::MeshRenderer(const std::string& path) : mesh_path(path) {
    material = std::make_unique<render::Material>();
}

MeshRenderer::~MeshRenderer() {
    GLOG_INFO("MeshRenderer::~MeshRenderer: destroying '{}', gpu_mesh_handle.index={}", mesh_path, gpu_mesh_handle_.index);
    if (material && ctx_) {
        GLOG_INFO("MeshRenderer::~MeshRenderer: destroying material GPU resources");
        material->destroy_gpu(ctx_);
    }
    if (gpu_mesh_handle_.is_valid() && ctx_) {
        GLOG_INFO("MeshRenderer::~MeshRenderer: destroying gpu_mesh");
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
    GLOG_INFO("MeshRenderer::~MeshRenderer: done '{}'", mesh_path);
}

render::Material* MeshRenderer::ensure_material() {
    if (!material) {
        material = std::make_unique<render::Material>();
    }
    return material.get();
}

void MeshRenderer::serialize(nlohmann::json& out) const {
    out["mesh_path"] = mesh_path;
    if (material) {
        nlohmann::json mat_json;
        material->serialize(mat_json);
        out["material"] = std::move(mat_json);
    }
}

void MeshRenderer::deserialize(const nlohmann::json& in) {
    mesh_path = in.value("mesh_path", "");
    ensure_material();
    auto it = in.find("material");
    if (it != in.end()) {
        material->deserialize(*it);
    }
}

render::IMesh* MeshRenderer::upload_to_gpu(render::RenderContext* ctx, const assets::MeshData* data,
                                             bool allow_while_running) {
    if (!ctx || !data || data->empty()) return nullptr;
    if (uploaded_.load(std::memory_order_acquire) && ctx_ == ctx) return gpu_mesh();

    // 渲染线程已接管 GL context 后，主线程不能再做 GL 上传；
    // 热重载等场景会由调用方先 pause_render_thread() 再上传。
    // allow_while_running=true 表示调用方已确保在渲染线程/命令队列中执行。
    if (!allow_while_running && ctx->is_running()) {
        return nullptr;
    }

    ctx_ = ctx;

    // 顶点布局：position(3) + normal(3) + tangent(3) + uv(2) + color(3)
    struct VertexGPU {
        float x, y, z;
        float nx, ny, nz;
        float tx, ty, tz;
        float u, v;
        float r, g, b;
    };

    std::vector<VertexGPU> verts;
    verts.reserve(data->vertices.size());
    for (const auto& v : data->vertices) {
        verts.push_back({
            v.position.x, v.position.y, v.position.z,
            v.normal.x, v.normal.y, v.normal.z,
            v.tangent.x, v.tangent.y, v.tangent.z,
            v.uv.x, v.uv.y,
            v.color.x, v.color.y, v.color.z
        });
    }

    // 先完成所有 GPU 资源创建和上传（包括材质），最后再发布 gpu_mesh_，
    // 避免主线程在材质纹理尚未就绪时就开始渲染。
    render::RHIMeshHandle mesh = ctx->create_mesh();
    render::IMesh* mesh_ptr = ctx->mesh(mesh);
    if (!mesh.is_valid() || !mesh_ptr) return nullptr;

    mesh_ptr->upload_vertices(verts.data(),
                          static_cast<uint32_t>(verts.size() * sizeof(VertexGPU)),
                          static_cast<uint32_t>(verts.size()));
    mesh_ptr->upload_indices(data->indices.data(),
                         static_cast<uint32_t>(data->indices.size() * sizeof(uint32_t)),
                         static_cast<uint32_t>(data->indices.size()));

    render::VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, render::VertexType::Float3, false, 0},
        {1, render::VertexType::Float3, false, 3 * sizeof(float)},
        {2, render::VertexType::Float3, false, 6 * sizeof(float)},
        {3, render::VertexType::Float2, false, 9 * sizeof(float)},
        {4, render::VertexType::Float3, false, 11 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);

    // 同时上传材质贴图
    if (material) {
        material->upload_to_gpu(ctx);
    }

    gpu_mesh_handle_ = mesh;
    uploaded_.store(true, std::memory_order_release);

    GLOG_INFO("MeshRenderer: uploaded '{}' to GPU ({} vertices, {} indices)",
              mesh_path, verts.size(), data->indices.size());
    return mesh_ptr;
}

} // namespace gryce_engine::components
