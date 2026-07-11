#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "components/component.h"
#include "assets/mesh_data.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/render2d.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// MeshRenderer — 3D 网格渲染组件
// 通过 res:/ 路径引用模型资源，上传 GPU 后渲染。
// ---------------------------------------------------------------------------
class MeshRenderer : public Component {
public:
    std::string mesh_path;
    std::unique_ptr<render::Material> material;

    MeshRenderer();
    explicit MeshRenderer(const std::string& path);
    ~MeshRenderer() override;

    const char* type() const override { return "MeshRenderer"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 确保 material 非空
    render::Material* ensure_material();

    // 上传/获取 GPU mesh（缓存，幂等）。
    // allow_while_running=true 允许在渲染线程运行时上传（供渲染系统内部使用）。
    render::IMesh* upload_to_gpu(render::RenderContext* ctx, const assets::MeshData* data,
                                 bool allow_while_running = false);

    // 当前已上传的 GPU mesh 句柄
    render::RHIMeshHandle gpu_mesh_handle() const {
        return gpu_mesh_handle_;
    }

    // 当前已上传的 GPU mesh（线程安全：仅当上传完全完成后才返回非空）
    render::IMesh* gpu_mesh() const {
        return (ctx_ && uploaded_.load(std::memory_order_acquire)) ? ctx_->mesh(gpu_mesh_handle_) : nullptr;
    }

private:
    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    // 渲染线程完成上传后设置，主线程通过 acquire 语义读取，
    // 保证 gpu_mesh_ 和 material 纹理指针对主线程可见。
    std::atomic<bool> uploaded_{false};
};

} // namespace gryce_engine::components
