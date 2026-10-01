#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/render2d.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// ParticleSystem3D — 3D 粒子发射器（CPU 模拟 + GPU 广告牌实例化渲染）
//
// 模拟在 on_update 内完成（编辑器 Play 模式与独立 exe 都由 Scene::update 驱动），
// 渲染由 RenderPipeline::render_particles 消费：组件把粒子写成每实例一条的
// 实例流，渲染线程上传到 mesh 的第二条顶点流并一次性实例化绘制。
//
// 广告牌展开放在顶点着色器里（用 view 矩阵的基向量），因此上传数据与相机无关，
// CPU 侧不需要取相机朝向。
// ---------------------------------------------------------------------------
class GRYCE_API ParticleSystem3D : public Component {
public:
    // 发射形状
    enum class Shape { Point = 0, Sphere = 1, Box = 2 };

    struct Particle {
        math::Vector3f position;
        math::Vector3f velocity;
        float age = 0.0f;
        float lifetime = 1.0f;
        float start_size = 0.1f;
        float end_size = 0.01f;
        render::Color start_color = render::Color::white();
        render::Color end_color = render::Color(1.0f, 1.0f, 1.0f, 0.0f);
        float rotation = 0.0f;
        float angular_velocity = 0.0f;
    };

    // 实例流布局：center(3) + size(1) + rotation(1) + color(4) = 36 字节/粒子。
    // 四边形几何由 6 顶点的静态网格提供（只上传一次），所以每颗粒子只需传
    // 一份数据；旧实现是把同样的 center/size/rotation/color 在 6 个顶点上重复
    // 写 6 遍（44 字节 × 6 = 264 字节/粒子），顶点着色器也要为每颗粒子跑 6 次。
    struct ParticleInstance {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float size = 0.0f;
        float rotation = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 0.0f;   // 0 表示未使用的槽位，片元着色器会 discard
    };

    // ---- 发射参数（序列化）-------------------------------------------------
    std::string texture_path;                        // 空 = 纯色方块
    bool loop = true;                                // false 时发射完一轮后停止
    bool play_on_awake = true;
    int max_particles = 256;
    float emission_rate = 10.0f;                     // 每秒发射数
    float lifetime_min = 1.0f;
    float lifetime_max = 2.0f;
    float speed_min = 1.0f;
    float speed_max = 3.0f;
    float start_size = 0.2f;                         // 世界单位直径
    float end_size = 0.05f;
    render::Color start_color = render::Color::white();
    render::Color end_color = render::Color(1.0f, 1.0f, 1.0f, 0.0f);
    bool additive = false;                           // true = 叠加混合（火焰/魔法）
    math::Vector3f emission_offset = math::Vector3f::zero();

    // ---- 运动与形态（序列化）-----------------------------------------------
    math::Vector3f acceleration = math::Vector3f(0.0f, -9.8f, 0.0f);
    math::Vector3f direction = math::Vector3f(0.0f, 1.0f, 0.0f);   // 发射主轴（局部空间）
    float spread_angle = 25.0f;                      // 方向锥半角（度），0 = 单向
    int shape = 0;                                   // Shape::Point / Sphere / Box
    float shape_radius = 0.1f;                       // Sphere 半径
    math::Vector3f shape_extents = math::Vector3f(0.1f, 0.1f, 0.1f); // Box 半尺寸
    float rotation_min = 0.0f;                       // 初始旋转（度）
    float rotation_max = 0.0f;
    float angular_velocity_min = 0.0f;               // 自转角速度（度/秒）
    float angular_velocity_max = 0.0f;
    float size_curve = 1.0f;                         // 尺寸插值指数，1 = 线性
    bool simulate_in_world_space = false;            // false = 粒子跟随发射器
    bool depth_test = true;                          // 关闭时作为叠加特效画在场景之上

    ParticleSystem3D();
    ~ParticleSystem3D() override;

    const char* type() const override { return "ParticleSystem3D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    void on_update(float dt) override;
    void on_destroy() override;

    // 立刻发射 count 个粒子
    void emit(int count);
    // 按剩余容量爆发一批（max_particles 的 1/8，至少 1 个）
    void burst();
    // 清空所有粒子
    void clear();
    int active_count() const;

    // 渲染侧：把当前粒子展开为实例流，并把上传命令投递到渲染线程。
    // 必须在主线程、render_scene 期间调用（每帧一次）。
    void prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    render::RHITextureHandle texture_handle() const { return texture_handle_; }

    // 本帧待绘制的实例数（供渲染侧决定 draw 的实例数量）
    uint32_t instance_count() const { return used_instances_; }

private:
    void emit_internal(int count, const math::Matrix4f& model);
    void spawn_particle(const math::Matrix4f& model);
    void build_instances(const math::Matrix4f& model);

    // 稠密数组：只保存存活粒子，死亡时用尾部元素交换填充（swap-remove），
    // 因此 size() 即存活数量，不需要扫描 inactive 槽位，也不存在空槽查找。
    std::vector<Particle> particles_;
    float emission_accumulator_ = 0.0f;
    float elapsed_ = 0.0f;
    bool emitting_ = true;

    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    // 槽位以 shared_ptr 捕获进命令队列，容量跨帧保留，避免每帧堆分配。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<ParticleInstance>> scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    uint32_t used_instances_ = 0;

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RHITextureHandle texture_handle_;
    render::RenderContext* ctx_ = nullptr;
    // 实例缓冲是否已按 max_particles 满容量分配（首帧一次性分配，之后只做子区间更新）
    std::atomic<bool> gpu_ready_{false};
    std::string texture_path_loaded_;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);

    static std::mt19937& rng();
    static float random_float(float min, float max);
};

} // namespace gryce_engine::components