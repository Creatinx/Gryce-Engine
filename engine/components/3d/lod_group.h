#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "components/component.h"
#include "export.h"
#include "assets/mesh_data.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/mesh.h"

namespace gryce_engine::render { class Material; }

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// LODGroup — 多级细节（Level of Detail）网格切换
//
// 与 Terrain 同类的"自包含发射网格"组件：组件自己经 assets::AssetManager 加载
// 当前 LOD 的网格，构建成 MeshData 后按 generation-guard 投递 GPU 上传命令，
// 渲染管线再把它并入不透明绘制列表（复用 pbr_shader_ + render_mesh_internal）。
//
// 字段语义（保持既有序列化字段名不变）：
//   - mesh_paths：LOD 网格路径，index 0 = 最高细节，随 index 递增细节下降。
//   - screen_size_thresholds：LOD 切换阈值。thresholds[k] 表示"从 LOD k 切换到
//     LOD k+1 的屏幕覆盖率上限"（因此应递减）。屏幕覆盖率由管线用相机距离与
//     包围球半径计算（radius / (distance * tan(fov/2))，相对半屏高归一化）。
//     若阈值数组短于 LOD 数，缺失项按 0.3/(k+1) 递推补齐。
//   - transition_duration：本引擎前向管线不提供逐帧 delta time，无法做时间驱动
//     的交叉淡入。这里把它映射为 LOD 切换的滞回带宽（band = clamp(0.05 +
//     transition_duration*0.5, 0.05, 0.5)）：阈值附近不抖动，避免"边缘反复横跳"。
//     这是一处已记录的妥协（选择了滞回带而非淡入）。
//   - active_lod：当前生效的 LOD 索引（由 update_lod 写入，可序列化/编辑）。
//
// 已知未接入项（与 Terrain 一致）：
//   - 阴影投射：阴影 pass 只遍历 MeshRenderer，LODGroup 网格不投影到 shadow map。
// ---------------------------------------------------------------------------
class GRYCE_API LODGroup : public Component {
public:
    std::vector<std::string> mesh_paths;
    std::vector<float> screen_size_thresholds;
    float transition_duration = 0.1f;
    int active_lod = 0;

    LODGroup();
    ~LODGroup() override;

    const char* type() const override { return "LODGroup"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // LOD 数量（mesh_paths 的条目数）
    int lod_count() const { return static_cast<int>(mesh_paths.size()); }

    // 依据屏幕覆盖率选择 LOD（含滞回）。返回写入 active_lod 的值。
    int update_lod(float screen_size);

    // -----------------------------------------------------------------------
    // GPU 化（渲染管线每帧调用）
    //
    // 仅当 active_lod 对应的网格路径变化（LOD 切换 / 路径编辑 / 资源热重载）时
    // 才重新加载 MeshData 并投递上传命令。静止物体每帧只做一次路径比对。
    // 约定与 Terrain 相同：顶点数据在主线程准备成 shared_ptr，上传命令按值捕获
    // 后由渲染线程执行（主线程在渲染线程运行期间不碰 GPU）。
    // -----------------------------------------------------------------------
    void prepare_gpu(render::RenderContext* ctx);

    // 丢弃 GPU 网格（资源热重载 / 上下文切换）；主线程调用安全。
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    const render::Material* material() const { return material_.get(); }
    render::Material* ensure_material();

    // 世界空间包围球（视锥剔除 + 屏幕覆盖率）：由当前已加载网格的局部 AABB
    // 与世界变换推出。尚未加载网格时退化为半径 0（保守交由视锥保留）。
    void world_bounds(const math::Matrix4f& world, math::Vector3f& center, float& radius) const;

private:
    int clamped_lod(int lod) const;
    float threshold_at(int index) const;

    // ---- GPU 状态 ----
    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::unique_ptr<render::Material> material_;
    // 当前待上传/已上传的 CPU 网格；按值捕获进上传命令，保证命令延迟执行时仍有效。
    std::shared_ptr<const assets::MeshData> geometry_;
    // 当前已加载的网格路径（用于判断是否需要重新加载）
    std::string geometry_path_loaded_;
    // 当前网格的局部包围盒（由 geometry_ 推出，供 world_bounds 使用）
    math::Vector3f local_min_ = math::Vector3f::zero();
    math::Vector3f local_max_ = math::Vector3f::zero();
    bool has_bounds_ = false;

    // 网格代次：主线程递增。queued_generation_ 记录"已投递上传命令"的代次，
    // 命令在渲染线程执行前不重复投递。初值 1 而 queued 为 0，保证首个网格一定上传。
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};
    std::atomic<bool> material_uploaded_{false};
    // 组件析构时置 false，延迟执行的异步上传命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components