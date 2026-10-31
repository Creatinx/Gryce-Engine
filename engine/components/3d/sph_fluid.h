#pragma once

#include <algorithm>
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
// SPHFluid3D — 3D SPH 粒子流体（CPU 模拟 + Surface Nets 连续表面重建）
//
// 模拟：Müller 2003（poly6 密度 / spiky 压力 / XSPH 速度平滑粘度）。邻居查找
// 用均匀空间哈希网格，但**不用哈希表**：粒子按格索引做一次计数排序，网格
// 数据落成两段连续数组（cell_start_ / sorted_），27 邻域遍历只读连续内存，
// 高密度下也不会出现哈希桶链表 + 指针追逐的抖动。模拟与渲染彻底解耦——
// 渲染侧看不到任何"粒子"。
//
// 渲染：每帧把粒子场 splat 成规则网格标量场（类似溅射密度），再用 Surface
// Nets 提取等值面：每个跨越等值面的单元生成一个顶点（位置取该单元 12 条边
// 交点的平均，等价于"用周围几颗粒子定位一个表面顶点"），相邻单元之间按
// 跨越的网格边连成四边形，输出一张三角网格。网格走与 Terrain/FluidSurface3D
// 相同的动态网格上传与透明列表绘制路径，因此能被 SSR/反射看到，也不再是
// 一粒粒的球。
//
// 性能口径：模拟 O(粒子数 × 邻居数)，重建 O(格点数 + 表面积)。重建分辨率
// 由 surface_resolution 控制（0 = 按容器尺寸与 spacing 自动推导）。
// ---------------------------------------------------------------------------
class GRYCE_API SPHFluid3D : public Component {
public:
    // ---- 体积与粒子（序列化）----------------------------------------------
    // 出生水域半尺寸（本地空间），按 spacing 填满
    math::Vector3f spawn_extents = math::Vector3f(0.35f, 0.3f, 0.35f);
    float spacing = 0.06f;          // 静止粒子间距（平滑半径 h = 2 * spacing）
    int max_particles = 1600;       // 粒子上限

    // ---- 求解参数（序列化）------------------------------------------------
    float rest_density = 1000.0f;   // 静止密度 ρ0（仅用于标定，求解按归一化密度 1.0）
    float stiffness = 8.0f;         // 旧状态方程常数（保留字段，PBF 不使用）
    float viscosity = 0.15f;        // XSPH 速度平滑系数 [0,1]
    int solver_iterations = 3;      // 密度约束投影迭代次数（越大越不可压、越慢）
    math::Vector3f gravity = math::Vector3f(0.0f, -9.8f, 0.0f);
    float buoyancy_strength = 6.0f; // 密度差浮力加速度上限（m/s²）
    float linear_drag = 0.08f;      // 向 flow_velocity 收敛的阻力系数
    math::Vector3f flow_velocity = math::Vector3f::zero(); // 均匀流场（造浪）

    // ---- 容器（序列化）----------------------------------------------------
    // 轴对齐盒子半尺寸（本地空间）；任一分量 <= 0 表示无容器
    math::Vector3f container_extents = math::Vector3f(0.9f, 0.7f, 0.9f);
    float wall_damping = 0.3f;      // 撞壁速度衰减

    // ---- 表面重建（序列化）------------------------------------------------
    // 等值面网格每轴格数上限；0 = 自动（按容器尺寸 / (spacing*2) 推导并夹取）
    int surface_resolution = 0;
    // 等值面阈值：内部 splat 权重归一化到 ≈1，0.5 处约在表面外半个间距
    float surface_iso = 0.5f;
    // 重建降频：每 N 帧重建一次网格（1 = 每帧）。模拟仍每帧推进。
    int surface_update_interval = 1;

    // ---- 外观（序列化）----------------------------------------------------
    math::Vector4f water_color = math::Vector4f(0.22f, 0.52f, 0.95f, 0.85f);
    float roughness = 0.08f;        // 水面粗糙度（低 = 清晰倒影）
    float metallic = 0.0f;

    // ---- 运行时开关 ---------------------------------------------------------
    bool simulate = true;
    bool reset_requested = false;

    SPHFluid3D();
    ~SPHFluid3D() override;

    const char* type() const override { return "SPHFluid3D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    void on_update(float dt) override;
    void on_destroy() override;

    // 重铺粒子（按 spawn_extents / spacing 网格填充，初速为零）
    void reset_particles();
    int active_count() const { return static_cast<int>(positions_.size()); }

    // 向水体注入扰动（局部坐标的球形冲量，供脚本/玩法调用）
    void splash(const math::Vector3f& local_center, float radius, float strength);

    // GPU 化（渲染管线每帧调用），约定与 Terrain/FluidSurface3D 一致
    void prepare_gpu(render::RenderContext* ctx);
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    const render::Material* material() const { return material_.get(); }
    render::Material* ensure_material();

    // 世界空间包围球（视锥剔除）
    void world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                      float& radius) const;

private:
    // ---- 模拟 --------------------------------------------------------------
    void simulate_step(float dt);
    void build_grid();
    void predict(float dt);
    void solve_density_constraints();
    void update_velocities(float dt);
    void apply_viscosity();
    void collide_walls();
    void accumulate_bounds();

    // ---- 表面重建 ----------------------------------------------------------
    void build_surface_field();
    void extract_surface(assets::MeshData& out) const;
    math::Vector3f surface_gradient_at(float gx, float gy, float gz) const;
    float sample_field_trilinear(float gx, float gy, float gz) const;
    int resolve_surface_resolution() const;

    float smooth_radius() const { return std::max(1e-3f, spacing * 2.0f); }

    // 模拟状态（SoA）
    std::vector<math::Vector3f> positions_;
    std::vector<math::Vector3f> positions_prev_;
    std::vector<math::Vector3f> velocities_;
    std::vector<float> densities_;
    std::vector<float> lambdas_;

    // 空间哈希：计数排序，无哈希表
    std::vector<int> cell_of_;        // 粒子 → 格线性下标
    std::vector<int> cell_start_;     // 格 → sorted_ 起始（长度 = 格数 + 1）
    std::vector<int> sorted_;         // 按格分桶后的粒子下标
    std::vector<int> cell_cursor_;    // 构建时的写游标（复用避免反复分配）
    math::Vector3f grid_origin_ = math::Vector3f::zero();
    int grid_dim_[3] = {1, 1, 1};
    float grid_cell_ = 0.12f;

    // 表面重建场
    std::vector<float> field_;        // 角点标量场（nx+1)*(ny+1)*(nz+1)
    mutable std::vector<int> cell_vertex_;  // 单元 → 顶点下标（-1 = 非活跃单元），提取期临时缓冲
    int field_dim_[3] = {0, 0, 0};    // 单元数（角点数 = +1）
    math::Vector3f field_origin_ = math::Vector3f::zero();
    float field_cell_ = 0.04f;

    math::Vector3f bounds_min_ = math::Vector3f::zero();
    math::Vector3f bounds_max_ = math::Vector3f::zero();

    float accumulator_ = 0.0f;
    int frame_counter_ = 0;

    // ---- GPU 状态（与 FluidSurface3D 同构）--------------------------------
    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::unique_ptr<render::Material> material_;
    std::shared_ptr<const assets::MeshData> geometry_;
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};
    std::atomic<bool> material_uploaded_{false};
    bool gpu_dirty_ = true;
    float last_width_hint_ = -1.0f;
    // 组件析构时置 false，延迟执行的异步上传命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components
