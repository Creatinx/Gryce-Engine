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
// FluidDomain3D — Mantaflow 式流体域：液体与气体在同一条速度场里共存
//
//   Blender 的流体是"一个域（domain）+ 若干流入（flow）"，液体和烟雾共用同一套
//   网格求解器，只是被表示成不同的量。这个组件照这个思路做：
//
//   速度场：MAC 交错网格（u 在 x 面心、v 在 y 面心、w 在 z 面心），格心存压力
//   与各相标量。交错网格是压力投影不产生棋盘格的前提，代价是三种分量的数组
//   尺寸各差一格。
//
//   液体：FLIP 粒子。
//     P2G 把粒子速度散播到面心（三线性权重）→ 求散度 → Jacobi 解压力泊松方程
//     → 减梯度投影成无散场 → G2P 把速度插值回粒子。粒子自己的速度保留
//     （FLIP，flip_ratio 混合 PIC），所以不像纯 PIC 那样被数值耗散成糖浆，
//     水花与飞溅能维持。液体格以外的格压力置零（自由表面 Dirichlet），
//     因此空气区域天然是"可压的"，水面不会像封闭容器那样被吸住。
//
//   气体：欧拉密度场。每步用半拉格朗日（RK2 回溯 + 三线性采样）平流，
//     浮力按密度给加速度，涡量约束把被数值耗散抹平的小涡重新放大 —— 没有这
//     一步烟雾只会缓慢摊平成一片雾，翻滚不起来。
//
//   空气为什么"也是流体"：域内没有液体的格子就是空气，它同样参与压力投影
//     （作为自由表面边界），同样有自己的速度。粒子的 G2P 读的是**这条共享
//     速度场**，所以液体在水下被水的速度带、在水面上被空气的速度带；外部
//     风（wind）直接写进空气格，烟雾随之被吹走、水面被吹起波纹。
//
//   渲染：液体走 Surface Nets（稠密粒子场提等值面），气体走**多层等值面**——
//     从密度场按几档 iso 各提一层半透明壳，靠层数与 alpha 叠出体积厚度。
//     比体积光线步进省太多，也不需要 3D 纹理通路；代价是侧视角能看到分层。
// ---------------------------------------------------------------------------
class GRYCE_API FluidDomain3D : public Component {
public:
    // ---- 域（序列化）------------------------------------------------------
    // 域半尺寸（本地空间，以组件原点为中心）
    math::Vector3f domain_extents = math::Vector3f(0.9f, 0.9f, 0.9f);
    // 最长轴格数。整个域按最长轴等分，其余轴按比例取格数（不强制立方体）。
    int resolution = 48;

    // ---- 液体（序列化）----------------------------------------------------
    bool liquid_enabled = true;
    math::Vector3f liquid_spawn_extents = math::Vector3f(0.55f, 0.22f, 0.55f);
    float liquid_spawn_fill = 0.62f;   // 出生块内的填充率（越小越稀疏）
    int max_particles = 9000;
    float flip_ratio = 0.92f;          // 1 = 纯 FLIP（保留全部粒子速度），0 = 纯 PIC
    math::Vector4f water_color = math::Vector4f(0.16f, 0.42f, 0.78f, 0.72f);
    float water_roughness = 0.03f;
    float water_metallic = 0.0f;
    // 表面等值面：粒子 splat 场内部≈1，0.5 落在表面外约一格
    float liquid_iso = 0.42f;

    // ---- 气体 / 空气（序列化）---------------------------------------------
    bool gas_enabled = true;
    // 气源（本地空间）：每帧在这个盒子里补充密度，模拟"烟囱口"
    math::Vector3f gas_source_center = math::Vector3f(-0.45f, -0.7f, 0.0f);
    math::Vector3f gas_source_extents = math::Vector3f(0.12f, 0.04f, 0.12f);
    float gas_emission = 2.2f;         // 每帧注入密度
    float gas_buoyancy = 3.2f;         // 密度 → 向上加速度
    float gas_dissipation = 0.35f;     // 每秒衰减系数
    float gas_vorticity = 2.4f;        // 涡量约束强度（0 = 关闭）
    math::Vector4f smoke_color = math::Vector4f(0.62f, 0.66f, 0.72f, 0.18f);
    // 多层等值面的层数（1~4）。层数越多体积感越强，几何也越多。
    int smoke_shells = 3;
    // 最内层壳的密度阈值。外层依次乘更小的系数。必须是绝对值而不是按每帧
    // 密度峰值取比例：烟柱升到高处会稀释到 0.1 量级，按峰值缩放的话稀薄的
    // 部分整段落在阈值以下，烟就只剩源口一小团。
    float smoke_iso = 0.30f;

    // ---- 环境（序列化）----------------------------------------------------
    math::Vector3f gravity = math::Vector3f(0.0f, -9.8f, 0.0f);
    // 外部风：直接写进空气格的速度，烟雾被吹走、水面被吹起波纹
    math::Vector3f wind = math::Vector3f::zero();
    int pressure_iterations = 32;
    int surface_update_interval = 1;   // 重建网格降频（1 = 每帧）

    // ---- 运行时 -----------------------------------------------------------
    bool simulate = true;

    FluidDomain3D();
    ~FluidDomain3D() override;

    const char* type() const override { return "FluidDomain3D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    void on_update(float dt) override;
    void on_destroy() override;

    void reset_fluid();
    int active_particles() const { return static_cast<int>(positions_.size()); }

    // 局部坐标球形冲量（供脚本/玩法调用）
    void splash(const math::Vector3f& local_center, float radius, float strength);

    // ---- GPU 化 -----------------------------------------------------------
    void prepare_gpu(render::RenderContext* ctx);
    void invalidate_gpu();

    // 一个域可能出多张网格：液体表面 1 层，烟雾 shells 层。
    // 渲管按层遍历，每层自带材质。
    int render_layer_count() const { return static_cast<int>(layers_.size()); }
    render::RHIMeshHandle layer_mesh(int index) const;
    const render::Material* layer_material(int index) const;

    void world_bounds(const math::Matrix4f& world, math::Vector3f& center, float& radius) const;

private:
    // ---- 网格 -------------------------------------------------------------
    struct Grid {
        int dim[3] = {1, 1, 1};       // 格心数
        math::Vector3f origin = math::Vector3f::zero();  // 域左下角（角点 0）
        float cell = 0.05f;
        std::vector<float> u, v, w;   // MAC 面心速度
        std::vector<float> prev_u, prev_v, prev_w;  // 投影前速度（FLIP 差量用）
        std::vector<float> p;         // 格心压力
        std::vector<float> div;       // 格心散度（投影临时）
        std::vector<float> frac;      // 格心液体体积分数 [0,1]
        std::vector<float> density;   // 格心烟雾密度
        std::vector<float> curl_x, curl_y, curl_z;  // 涡量
        std::vector<float> tmp;       // 平流临时
        std::vector<float> p_next;    // frac 平滑临时
    };

    // ---- 模拟 -------------------------------------------------------------
    void setup_grid();
    void simulate_step(float dt);
    void emit_gas(float dt);
    void integrate_particles(float dt);
    void particles_to_grid();
    void apply_buoyancy(float dt);
    void apply_wind(float dt);
    void apply_vorticity(float dt);
    void advect_density(float dt);
    void smooth_frac();
    void project(float dt);
    void grid_to_particles();
    void build_particle_grid();
    void solve_particle_spacing();
    void collide_domain();
    void dissipate_gas(float dt);

    // MAC 交错网格的三种分量下标。尺寸分别是
    //   u: (nx+1) * ny   * nz
    //   v: nx     * (ny+1) * nz
    //   w: nx     * ny   * (nz+1)
    int idx_c(int x, int y, int z) const { return (z * grid_.dim[1] + y) * grid_.dim[0] + x; }
    int idx_u(int x, int y, int z) const {
        return (z * grid_.dim[1] + y) * (grid_.dim[0] + 1) + x;
    }
    int idx_v(int x, int y, int z) const {
        return (z * (grid_.dim[1] + 1) + y) * grid_.dim[0] + x;
    }
    int idx_w(int x, int y, int z) const {
        return (z * grid_.dim[1] + y) * grid_.dim[0] + x;
    }

    // 采样（格心 / 面心三线性）
    math::Vector3f sample_cell_velocity(float gx, float gy, float gz) const;
    float sample_cell_scalar(const std::vector<float>& field, float gx, float gy, float gz) const;
    bool in_domain(const math::Vector3f& p) const;

    // ---- 表面重建 ---------------------------------------------------------
    void rebuild_surface();
    void build_liquid_field();
    void build_smoke_field();

    // ---- GPU --------------------------------------------------------------
    struct Layer {
        render::RHIMeshHandle mesh;
        std::unique_ptr<render::Material> material;
        std::shared_ptr<const assets::MeshData> geometry;
        uint64_t generation = 1;
        std::atomic<uint64_t> queued{0};
        std::atomic<bool> uploaded{false};
        bool dirty = true;
    };

    render::RenderContext* ctx_ = nullptr;
    std::vector<std::unique_ptr<Layer>> layers_;
    uint32_t liquid_layer_ = 0;
    uint32_t smoke_layer_first_ = 1;

    // ---- 状态 -------------------------------------------------------------
    Grid grid_;
    std::vector<math::Vector3f> positions_;
    std::vector<math::Vector3f> velocities_;

    // 粒子邻域：计数排序空间哈希（与 SPHFluid3D 同一套做法，无哈希表）
    std::vector<int> cell_of_;
    std::vector<int> cell_start_;
    std::vector<int> sorted_;
    std::vector<int> cell_cursor_;
    math::Vector3f pgrid_origin_ = math::Vector3f::zero();
    int pgrid_dim_[3] = {1, 1, 1};
    float pgrid_cell_ = 0.02f;

    // 稠密粒子场（表面重建用）
    std::vector<float> liquid_field_;
    std::vector<float> smoke_field_;
    int field_dim_[3] = {0, 0, 0};
    math::Vector3f field_origin_ = math::Vector3f::zero();
    float field_cell_ = 0.05f;

    float spacing_ = 0.02f;      // 实际播种间距（受 max_particles 影响会被放大）
    float accumulator_ = 0.0f;
    int frame_counter_ = 0;
    bool initialized_ = false;

    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components
