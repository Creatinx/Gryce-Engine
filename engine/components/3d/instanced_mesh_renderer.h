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
    // 内联材质（与 MeshRenderer 的 "material" 字段同构，序列化 key 同为
    // "material"）。material_path 为空时用它。之前只有 material_path，而项目里
    // 没有 .mat 文件、MeshRenderer 一律用内联材质 —— 等于实例化物体压根没法配
    // 材质。绘制侧 material_ 为空会**跳过 bind**，沿用上一个物体的 uniform，
    // 画面上表现为一堆黑块，很难联想到是材质没绑。
    // 名字带 inline_ 前缀是因为 material() 已被访问器占用。
    nlohmann::json inline_material;
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

    // ---- GPU 剔除（B1x）----------------------------------------------------
    // 这两个流由 build_instance_matrices 同步产出，与 cached_instances_ 同一次
    // 重建、同一份缓存（shared_ptr 保证命令队列执行时仍有效）。
    // GPU 剔除 compute 需要每实例世界包围球做视锥 + HiZ 测试，缺失就只能走
    // CPU 兜底或整批直接绘制。
    const std::shared_ptr<std::vector<math::Matrix4f>>& instance_matrices_gpu() const {
        return cached_instances_;
    }
    const std::shared_ptr<std::vector<math::Vector4f>>& instance_spheres_gpu() const {
        return cached_spheres_;
    }

private:
    void build_instance_matrices(const math::Matrix4f& world);

    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<math::Matrix4f>> scratch_[k_scratch_slots];
    // 包围球流用同样三槽：与矩阵同一次重建、同一个槽位索引切换。
    std::shared_ptr<std::vector<math::Vector4f>> sphere_scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    // 本帧上传用的实例流（实例参数未变时复用上一次构建的槽位）
    std::shared_ptr<std::vector<math::Matrix4f>> cached_instances_;
    // 每实例世界包围球（xyz=世界中心, w=半径），供 GPU 剔除 compute 使用。
    // 与 cached_instances_ 同批产出；原型网格无边界时半径给极大值（必定可见）。
    std::shared_ptr<std::vector<math::Vector4f>> cached_spheres_;
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

    // 实例流是否需要重新上传到 GPU（主线程置位 → 渲染线程消费）。
    //
    // ★ 必须"按需上传"：实例矩阵本身已经用 params_changed 做了脏检查，但上传
    //   原先无条件每帧执行 —— D3D12 侧 upload_instances 是「Release 旧缓冲 +
    //   新建 + immediate_submit_copy（提交命令列表并等 GPU fence）」，于是**每帧
    //   插一次 GPU 全同步**到渲染线程中间，既把帧率压到 ~2.5fps，也扰乱了主
    //   命令列表的提交顺序。GL/Vulkan 同样是无谓的每帧重传。
    std::shared_ptr<std::atomic<bool>> instance_upload_dirty_ =
        std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components