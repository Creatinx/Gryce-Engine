#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// SPHFluid3D — 3D SPH 流体（CPU 模拟 + 粒子广告牌实例化渲染）
//
// 求解器为经典 Müller 2003（"Particle-Based Fluid Simulation for Interactive
// Applications"）：poly6 核算密度、spiky 梯度算压力、Laplacian 算粘度，
// 邻居查找用均匀空间哈希网格（格子边长 = 平滑半径 h）。
//
// 参数命名对齐 BuoyancyArea2D：fluid_density / linear_drag / flow_velocity。
// 浮力实现为密度差驱动：局部密度低于静止密度的粒子（气泡/泡沫）受额外
// 向上加速度，产生水面翻腾与气泡上浮的表现。
//
// 模拟在 on_update 内完成（固定子步长），渲染由 RenderPipeline 的粒子
// pass 消费：组件把粒子写成与 ParticleSystem3D 相同布局的实例流
// （center(3)+size(1)+rotation(1)+color(4) = 36 字节/实例），共用同一份
// 粒子着色器与广告牌原型网格。
// ---------------------------------------------------------------------------
class GRYCE_API SPHFluid3D : public Component {
public:
    // 实例流布局：与 ParticleSystem3D::ParticleInstance 完全一致
    struct FluidInstance {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float size = 0.0f;
        float rotation = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 0.0f;   // 0 表示未使用槽位，片元着色器 discard
    };

    // ---- 体积与粒子（序列化）----------------------------------------------
    math::Vector3f spawn_extents = math::Vector3f(0.4f, 0.4f, 0.4f); // 出生水域半尺寸（本地）
    float spacing = 0.06f;                        // 静止粒子间距（也决定 h = 2*spacing）
    int max_particles = 800;                      // 粒子上限（出生按网格填充，不超过它）

    // ---- 材质参数（序列化）------------------------------------------------
    float rest_density = 1000.0f;                 // 静止密度 ρ0
    float stiffness = 8.0f;                       // 气体状态方程常数 k（已按尺度调好）
    float viscosity = 0.15f;                      // 粘度系数 μ
    math::Vector3f gravity = math::Vector3f(0.0f, -9.8f, 0.0f);

    // ---- 浮力 / 流场（序列化，对齐 BuoyancyArea2D 命名）--------------------
    float buoyancy_strength = 6.0f;               // 密度差浮力加速度上限（m/s²）
    float linear_drag = 0.08f;                    // 全局线性阻力（相对 flow_velocity）
    math::Vector3f flow_velocity = math::Vector3f::zero(); // 均匀流场速度（造浪用）

    // ---- 容器（序列化）-----------------------------------------------------
    // 轴对齐盒子（本地空间半尺寸），粒子被约束在盒内； extents<=0 表示无容器
    math::Vector3f container_extents = math::Vector3f(1.0f, 0.8f, 1.0f);
    float wall_damping = 0.3f;                    // 撞壁速度衰减（0=完全反弹能量损失大）

    // ---- 渲染（序列化）-----------------------------------------------------
    float particle_size = 0.09f;                  // 广告牌直径（世界单位）
    render::Color water_color = render::Color(0.25f, 0.55f, 0.95f, 0.85f);
    bool speed_tint = true;                       // 按速度混入白色（浪尖发白）

    // ---- 运行时开关 ----------------------------------------------------------
    bool simulate = true;                         // false 时冻结模拟只渲染
    bool reset_requested = false;                 // 置 true 后下一帧重铺粒子

    SPHFluid3D();
    ~SPHFluid3D() override;

    const char* type() const override { return "SPHFluid3D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    void on_update(float dt) override;
    void on_destroy() override;

    // 重铺粒子：按 spawn_extents 与 spacing 网格填充，初速为零
    void reset_particles();

    int active_count() const { return static_cast<int>(positions_.size()); }

    // 渲染侧：展开实例流并投递上传命令（主线程、render_scene 期间、每帧一次）
    void prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model);
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    render::RHITextureHandle texture_handle() const { return texture_handle_; }
    uint32_t instance_count() const { return used_instances_; }
    // 粒子 pass 绘制参数（与 ParticleSystem3D 的对应字段同义）
    static constexpr bool additive = false;
    static constexpr bool depth_test = true;

private:
    void simulate_step(float dt);
    void build_neighbors();
    void compute_density_pressure();
    void compute_forces();
    void integrate(float dt);
    void apply_viscosity();
    void collide_walls();
    void build_instances(const math::Matrix4f& model);

    // 模拟状态（SoA）
    std::vector<math::Vector3f> positions_;
    std::vector<math::Vector3f> velocities_;
    std::vector<math::Vector3f> forces_;
    std::vector<float> densities_;
    std::vector<float> pressures_;
    // 空间哈希：cell 索引 → 粒子索引列表；grid_map_ 是 cell key → cell 下标
    std::unordered_map<uint64_t, int> grid_map_;
    std::vector<std::vector<int>> grid_cells_;
    std::vector<int> grid_origin_;   // 每粒子所在 cell 下标
    float h_ = 0.12f;                // 平滑半径（reset 时按 spacing 更新）

    float fixed_dt_ = 1.0f / 120.0f;
    float accumulator_ = 0.0f;

    // 三槽环形实例缓冲（与 ParticleSystem3D 同样的跨帧安全策略）
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<FluidInstance>> scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    uint32_t used_instances_ = 0;

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RHITextureHandle texture_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::atomic<bool> gpu_ready_{false};
    std::shared_ptr<bool> alive_token_ = std::make_shared<bool>(true);
};

} // namespace gryce_engine::components
