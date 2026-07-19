#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "components/component.h"
#include "assets/skinned_mesh_data.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "render/material.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// SkinnedMeshRenderer — 骨骼蒙皮网格渲染组件
//
// 与 MeshRenderer 平行的独立组件（独立顶点布局，不干扰普通 mesh 路径）：
// - model_path 引用带 skin 的模型（.gltf/.fbx），CPU 数据为 SkinnedModelData
// - 顶点布局在 MeshVertex 后追加 bone ids（location 5）/ weights（location 6）
// - palette 由 AnimatorSystem 每帧在主线程求值后 set_palette() 注入；
//   RenderPipeline 绘制时经 RenderContext::set_uniform_mat4_array 推到渲染线程
//
// 生命周期/上传模式与 MeshRenderer 相同：alive_token 防 UAF，
// upload_to_gpu 在渲染线程命令中执行（allow_while_running=true）。
// ---------------------------------------------------------------------------
class SkinnedMeshRenderer : public Component {
public:
    std::string model_path;
    std::unique_ptr<render::Material> material;

    // 播放状态（AnimatorSystem 驱动）
    std::string clip_name;          // 空 = 第一个 clip
    bool playing = true;
    bool loop = true;
    float speed = 1.0f;
    float time = 0.0f;              // 秒

    SkinnedMeshRenderer();
    explicit SkinnedMeshRenderer(const std::string& path);
    ~SkinnedMeshRenderer() override;

    const char* type() const override { return "SkinnedMeshRenderer"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    render::Material* ensure_material();

    // CPU 模型数据（AssetManager 缓存的共享所有权）
    void set_model(std::shared_ptr<assets::SkinnedModelData> model);
    const std::shared_ptr<assets::SkinnedModelData>& model() const { return model_; }

    // 解析当前 clip：clip_name 匹配优先，否则第一个；无动画返回 nullptr
    const animation::AnimationClip* resolve_clip() const;

    // 当前帧 palette（AnimatorSystem 每帧注入新分配的 shared_ptr；
    // 渲染命令按值捕获后，本组件再替换指针对渲染线程无影响）
    void set_palette(std::shared_ptr<const std::vector<math::Matrix4f>> palette);
    const std::shared_ptr<const std::vector<math::Matrix4f>>& palette() const { return palette_; }

    // 上传/获取 GPU mesh（缓存，幂等）。语义同 MeshRenderer::upload_to_gpu。
    render::IMesh* upload_to_gpu(render::RenderContext* ctx, bool allow_while_running = false);

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    render::IMesh* gpu_mesh() const {
        return (ctx_ && uploaded_.load(std::memory_order_acquire)) ? ctx_->mesh(gpu_mesh_handle_) : nullptr;
    }

    // 见 MeshRenderer::alive_token
    std::shared_ptr<std::atomic<bool>> alive_token() const { return alive_token_; }

private:
    std::shared_ptr<assets::SkinnedModelData> model_;
    std::shared_ptr<const std::vector<math::Matrix4f>> palette_;

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::atomic<bool> uploaded_{false};
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components
