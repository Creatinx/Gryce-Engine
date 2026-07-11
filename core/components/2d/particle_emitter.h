#pragma once

#include <random>
#include <string>
#include <vector>

#include "components/2d/component_2d.h"
#include "render/render2d.h"

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// ParticleEmitter2D — 2D 粒子发射器
// 支持持续发射、瞬间爆发、颜色/尺寸插值、重力、生命周期。
// 默认用于尘土、碎屑等效果。
// ---------------------------------------------------------------------------
class ParticleEmitter2D : public Component2D {
public:
    struct Particle {
        math::Vector2f position;
        math::Vector2f velocity;
        math::Vector2f acceleration;
        float age = 0.0f;
        float lifetime = 1.0f;
        render::Color start_color = render::Color::white();
        render::Color end_color = render::Color(1.0f, 1.0f, 1.0f, 0.0f);
        float start_size = 8.0f;
        float end_size = 2.0f;
        float rotation = 0.0f;
        float angular_velocity = 0.0f;
        bool active = false;
    };

    // 发射参数
    float emission_rate = 0.0f;          // 每秒持续发射粒子数，0 表示不持续发射
    int max_particles = 256;

    // 每次爆发的数量范围（调用 burst() 时使用）
    int burst_min = 8;
    int burst_max = 16;

    // 生命周期（秒）
    float lifetime_min = 0.4f;
    float lifetime_max = 0.8f;

    // 速度（世界单位/秒）
    float velocity_min = 50.0f;
    float velocity_max = 150.0f;

    // 发射方向（弧度，0 表示向右 +X）
    float direction_min = -3.14159265f * 0.25f;
    float direction_max =  3.14159265f * 0.25f;

    // 加速度/重力（世界单位/秒²）
    math::Vector2f acceleration = math::Vector2f(0.0f, 0.0f);

    // 颜色插值
    render::Color start_color = render::Color::white();
    render::Color end_color = render::Color(1.0f, 1.0f, 1.0f, 0.0f);

    // 尺寸插值（像素）
    float start_size = 8.0f;
    float end_size = 2.0f;

    // 旋转
    float rotation_min = 0.0f;
    float rotation_max = 0.0f;
    float angular_velocity_min = -180.0f;
    float angular_velocity_max =  180.0f;

    // 可选贴图（空表示纯色矩形）
    std::string texture_path;

    // 是否使用相加混合（TODO：等 RHI 暴露 blend mode 后实现）
    bool additive = false;

    // 发射原点偏移（相对于 owner 的 Transform）
    math::Vector2f emission_offset = math::Vector2f::zero();

    ParticleEmitter2D() = default;

    const char* type() const override { return "ParticleEmitter2D"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    void on_update(float dt) override;
    void draw(render::IRenderer2D* renderer) override;

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        int active = active_count();
        hash_combine(h, static_cast<uint64_t>(active));
        // 只要有粒子就认为画面在变化；用第一颗活跃粒子作为代表
        for (const auto& p : particles_) {
            if (p.active) {
                hash_combine(h, hash_float(p.position.x));
                hash_combine(h, hash_float(p.position.y));
                hash_combine(h, hash_float(p.age));
                hash_combine(h, hash_float(p.start_size + (p.end_size - p.start_size) * (p.age / p.lifetime)));
                break;
            }
        }
        return h;
    }

    // 立刻发射 count 个粒子
    void emit(int count);
    // 按 burst_min/burst_max 随机爆发
    void burst();
    // 清空所有粒子
    void clear();

    // 当前活跃粒子数
    int active_count() const;

private:
    void spawn_particle();
    math::Vector2f emission_origin() const;

    std::vector<Particle> particles_;
    float emission_accumulator_ = 0.0f;

    // 运行时加载的贴图（不序列化）
    mutable render::ITexture* texture_ = nullptr;

    static std::mt19937& rng();
    static float random_float(float min, float max);
    static int random_int(int min, int max);
};

} // namespace gryce_engine::components::d2
