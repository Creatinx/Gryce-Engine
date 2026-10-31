#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "assets/mesh_data.h"
#include "components/component.h"
#include "export.h"
#include "math/math.h"
#include "render/mesh.h"
#include "render/render_context.h"

namespace gryce_engine::render {
class Material;
}

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// FluidSurface3D — 高度场流体组件（浅水方程 / 虚拟管道法）
//
// 用 N×N 高度场网格描述水体：每格存水柱高度与四向出流流量，按虚拟管道法
// （Mei et al. 2004）显式积分。复杂度 O(格数)，无邻居搜索、无粒子，高分辨率
// （256×256 = 6.5 万格）每帧仍只需毫秒级 CPU——这是相对粒子 SPH 的核心优势。
//
// 水面直接用高度场构建的动态网格渲染（顶点 Y = 水柱高度，法线由高度梯度
// 解析求出），复用 PBR 管线与常规透明物体路径，观感是连续水面而非粒子云。
//
// GPU 化与 Terrain 完全同构：CPU 侧构建 MeshData，投递上传命令由渲染线程
// 执行 upload_vertices 原地重传（顶点数不变），静止/暂停时零上传。
// ---------------------------------------------------------------------------
class GRYCE_API FluidSurface3D : public Component {
public:
    // 水面平面尺寸（世界单位，局部空间以原点为中心）
    float width = 2.0f;
    float depth = 2.0f;

    // 每边格数（顶点数 = resolution + 1）。模拟与网格同分辨率；
    // 128 即 16384 格，模拟 <1ms、上传约 0.9MB/帧，普通桌面可实时。
    int resolution = 96;

    // 静水时的水柱高度（水面在局部 Y = water_level）
    float water_level = 0.35f;

    // 速度阻尼系数 [0,1)：每秒衰减比例。对齐 BuoyancyArea2D 的 linear_drag 语义。
    float linear_drag = 0.05f;

    // 水平流场（世界单位/秒²的倾斜驱动）：水体持续被推向该方向并在容器边
    // 堆积回涌，与 BuoyancyArea2D 的 flow_velocity 语义对齐（y 分量忽略）。
    math::Vector3f flow_velocity = math::Vector3f::zero();

    // 扰动强度 [0,1]：每秒随机落点次数的期望（按格归一），0 = 平静水面。
    float rain_rate = 0.0f;

    // 扰动幅度（对水柱高度的瞬时增量，米）
    float rain_strength = 0.02f;

    // 水色（rgb + 不透明度 a，材质按 Blend 模式渲染）
    math::Vector4f water_color = math::Vector4f(0.22f, 0.52f, 0.95f, 0.72f);

    // false 时冻结模拟只渲染（暂停/回放）
    bool simulate = true;

    FluidSurface3D();
    ~FluidSurface3D() override;

    const char* type() const override { return "FluidSurface3D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 丢一个方块进水里（指定格心 + 半径内高斯凹陷/抬升），供脚本与玩法调用
    void splash(float local_x, float local_z, float radius, float strength);

    // 单步推进（内部按固定子步长折算 dt，每帧最多 k_max_substeps 步）
    void on_update(float dt) override;

    // GPU 化（渲染管线每帧调用），约定与 Terrain 一致
    void prepare_gpu(render::RenderContext* ctx);
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    const render::Material* material() const { return material_.get(); }
    render::Material* ensure_material();

    // 世界空间包围球（视锥剔除），由尺寸推出，不查资源 AABB
    void world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                      float& radius) const;

private:
    // 水柱高度与四向出流流量（格序 = z * stride + x）
    std::vector<float> height_;
    std::vector<float> flux_l_;
    std::vector<float> flux_r_;
    std::vector<float> flux_t_;
    std::vector<float> flux_b_;

    void ensure_fields();
    void simulate_step(float dt);
    void reset_to_rest();
    math::Vector3f compute_normal(int x, int z) const;
    assets::MeshData build_mesh_data() const;

    int stride() const { return std::max(2, resolution + 1); }

    // ---- GPU 状态（与 Terrain 同构）----
    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::unique_ptr<render::Material> material_;
    std::shared_ptr<const assets::MeshData> geometry_;
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};
    std::atomic<bool> material_uploaded_{false};
    // 模拟推进或参数变化后置 true：下次 prepare_gpu 重建网格
    bool gpu_dirty_ = true;
    // 参数快照：编辑器直接改公开字段也能触发网格重建
    float last_width_ = 0.0f;
    float last_depth_ = 0.0f;
    int last_resolution_ = -1;
    float last_water_level_ = -1.0f;
    // 组件析构时置 false，延迟执行的异步上传命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
    // 随机扰动的确定性序列
    uint32_t rain_seed_ = 1;
};

} // namespace gryce_engine::components
