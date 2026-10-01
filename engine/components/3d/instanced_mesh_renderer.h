#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "assets/mesh_data.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/mesh.h"

namespace gryce_engine::render {
class Material;
} // namespace gryce_engine::render

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// InstancedMeshRenderer — GPU 实例化渲染（植被 / 碎石 / 人群）
//
// 几何：按 mesh_path 加载一次模型，去掉索引展开成平坦顶点流上传（一种原型）。
// 实例：按 seed + spacing 确定性生成 instance_count 个变换（mat4，每实例 64 字节），
//       走 IMesh 的第二条输入流（inputRate = instance）。
// 绘制：一次 draw 调用画完所有实例（GL 走 glDrawArraysInstanced、Vulkan 走
//       vkCmdDraw 的 instanceCount），因此实例数增加不会增加 draw call 数。
//
// 注意：几何必须去索引。GL 的 draw() 优先走索引分支（glDrawElements，非实例化），
// 上传 indices 会让实例化静默失效，所以只上传展开后的顶点流。
// ---------------------------------------------------------------------------
class GRYCE_API InstancedMeshRenderer : public Component {
public:
    std::string mesh_path;
    std::string material_path;
    int instance_count = 1;
    float spacing = 1.0f;
    int seed = 0;

    InstancedMeshRenderer() = default;
    ~InstancedMeshRenderer() override;

    const char* type() const override { return "InstancedMeshRenderer"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // ---- GPU 侧（由 RenderPipeline::collect_instanced 每帧调用一次）---------
    // 生成实例变换流并把上传命令投递到渲染线程。必须在主线程、render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& world);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    // 本帧待绘制的实例数（供渲染侧判断是否有内容可画）
    uint32_t instance_count_gpu() const { return instance_count_; }

    const render::Material* material() const { return material_.get(); }

private:
    void build_instance_matrices(const math::Matrix4f& world);

    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<math::Matrix4f>> scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    // 本帧上传用的实例流（实例参数未变时复用上一次构建的槽位）
    std::shared_ptr<std::vector<math::Matrix4f>> cached_instances_;
    uint32_t instance_count_ = 0;

    // 主线程加载的资源（渲染线程不碰 AssetManager）
    std::shared_ptr<const assets::MeshData> geometry_;
    // 去索引展开后的平坦顶点流（与 geometry_ 同步重建，按 shared_ptr 交给命令队列）
    std::shared_ptr<std::vector<assets::MeshVertex>> flat_vertices_;
    std::string geometry_path_loaded_;
    std::string material_path_loaded_;
    std::unique_ptr<render::Material> material_;
    std::atomic<bool> material_uploaded_{false};

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;

    // 脏检查：世界矩阵或实例参数任一变化才重建实例流
    bool has_cache_ = false;
    math::Matrix4f last_world_ = math::Matrix4f::identity();
    int last_count_ = -1;
    float last_spacing_ = 0.0f;
    int last_seed_ = 0;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components