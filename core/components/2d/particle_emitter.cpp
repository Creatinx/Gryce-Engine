#include "components/2d/particle_emitter.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "components/transform.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components::d2 {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_deg2rad = k_pi / 180.0f;

render::Color lerp_color(const render::Color& a, const render::Color& b, float t) {
    return render::Color(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    );
}

} // namespace

std::mt19937& ParticleEmitter2D::rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

float ParticleEmitter2D::random_float(float min, float max) {
    if (min >= max) return min;
    std::uniform_real_distribution<float> dist(min, max);
    return dist(rng());
}

int ParticleEmitter2D::random_int(int min, int max) {
    if (min >= max) return min;
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng());
}

math::Vector2f ParticleEmitter2D::emission_origin() const {
    math::Vector2f origin = position();
    math::Vector2f s = scale();
    return math::Vector2f(
        origin.x + emission_offset.x * s.x,
        origin.y + emission_offset.y * s.y
    );
}

void ParticleEmitter2D::spawn_particle() {
    Particle p;
    p.position = emission_origin();

    float dir = random_float(direction_min, direction_max);
    float speed = random_float(velocity_min, velocity_max);
    p.velocity = math::Vector2f(std::cos(dir) * speed, std::sin(dir) * speed);

    p.acceleration = acceleration;
    p.lifetime = random_float(lifetime_min, lifetime_max);
    p.age = 0.0f;
    p.start_color = start_color;
    p.end_color = end_color;
    p.start_size = start_size;
    p.end_size = end_size;
    p.rotation = random_float(rotation_min, rotation_max);
    p.angular_velocity = random_float(angular_velocity_min, angular_velocity_max) * k_deg2rad;
    p.active = true;

    if (static_cast<int>(particles_.size()) < max_particles) {
        particles_.push_back(p);
        return;
    }

    // 容器已满，复用最老的非活跃粒子槽位
    auto it = std::find_if(particles_.begin(), particles_.end(),
                           [](const Particle& p) { return !p.active; });
    if (it != particles_.end()) {
        *it = p;
    }
}

void ParticleEmitter2D::emit(int count) {
    int active = active_count();
    int slots = max_particles - active;
    if (slots <= 0) return;
    count = std::min(count, slots);
    for (int i = 0; i < count; ++i) {
        spawn_particle();
    }
}

void ParticleEmitter2D::burst() {
    emit(random_int(burst_min, burst_max));
}

void ParticleEmitter2D::clear() {
    particles_.clear();
}

int ParticleEmitter2D::active_count() const {
    int n = 0;
    for (const auto& p : particles_) {
        if (p.active) ++n;
    }
    return n;
}

void ParticleEmitter2D::on_update(float dt) {
    if (!enabled) return;

    // 持续发射
    if (emission_rate > 0.0f) {
        emission_accumulator_ += emission_rate * dt;
        int to_emit = static_cast<int>(emission_accumulator_);
        if (to_emit > 0) {
            emission_accumulator_ -= static_cast<float>(to_emit);
            emit(to_emit);
        }
    }

    // 更新粒子
    for (auto& p : particles_) {
        if (!p.active) continue;
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.active = false;
            continue;
        }
        p.velocity += p.acceleration * dt;
        p.position += p.velocity * dt;
        p.rotation += p.angular_velocity * dt;
    }

    // 移除死亡粒子，避免容器无限增长
    particles_.erase(
        std::remove_if(particles_.begin(), particles_.end(),
                       [](const Particle& p) { return !p.active; }),
        particles_.end());
}

void ParticleEmitter2D::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer) return;

    // TODO: 贴图加载与按贴图绘制；目前粒子使用纯色矩形，性能更好且足够演示。
    (void)texture_path;
    (void)texture_;
    (void)additive;

    for (const auto& p : particles_) {
        if (!p.active) continue;

        float t = p.age / p.lifetime;
        render::Color color = lerp_color(p.start_color, p.end_color, t);
        float size = p.start_size + (p.end_size - p.start_size) * t;
        float half = size * 0.5f;

        if (std::abs(p.rotation) < 0.001f) {
            renderer->draw_rect(p.position.x - half, p.position.y - half,
                                size, size, color);
        } else {
            float c = std::cos(p.rotation);
            float s = std::sin(p.rotation);
            math::Vector2f corners[4] = {
                math::Vector2f(-half * c - -half * s, -half * s + -half * c),
                math::Vector2f( half * c - -half * s,  half * s + -half * c),
                math::Vector2f( half * c -  half * s,  half * s +  half * c),
                math::Vector2f(-half * c -  half * s, -half * s +  half * c)
            };
            std::vector<math::Vector2f> poly;
            for (int i = 0; i < 4; ++i) {
                poly.emplace_back(p.position.x + corners[i].x,
                                  p.position.y + corners[i].y);
            }
            renderer->draw_polygon(poly, color);
        }
    }
}

void ParticleEmitter2D::serialize(nlohmann::json& out) const {
    Component2D::serialize_base(out);
    out["emission_rate"] = emission_rate;
    out["max_particles"] = max_particles;
    out["burst_min"] = burst_min;
    out["burst_max"] = burst_max;
    out["lifetime_min"] = lifetime_min;
    out["lifetime_max"] = lifetime_max;
    out["velocity_min"] = velocity_min;
    out["velocity_max"] = velocity_max;
    out["direction_min"] = direction_min;
    out["direction_max"] = direction_max;
    out["acceleration"] = { acceleration.x, acceleration.y };
    out["start_color"] = { start_color.r, start_color.g, start_color.b, start_color.a };
    out["end_color"] = { end_color.r, end_color.g, end_color.b, end_color.a };
    out["start_size"] = start_size;
    out["end_size"] = end_size;
    out["rotation_min"] = rotation_min;
    out["rotation_max"] = rotation_max;
    out["angular_velocity_min"] = angular_velocity_min;
    out["angular_velocity_max"] = angular_velocity_max;
    out["texture_path"] = texture_path;
    out["additive"] = additive;
    out["emission_offset"] = { emission_offset.x, emission_offset.y };
}

void ParticleEmitter2D::deserialize(const nlohmann::json& in) {
    Component2D::deserialize_base(in);
    emission_rate = in.value("emission_rate", 0.0f);
    max_particles = in.value("max_particles", 256);
    burst_min = in.value("burst_min", 8);
    burst_max = in.value("burst_max", 16);
    lifetime_min = in.value("lifetime_min", 0.4f);
    lifetime_max = in.value("lifetime_max", 0.8f);
    velocity_min = in.value("velocity_min", 50.0f);
    velocity_max = in.value("velocity_max", 150.0f);
    direction_min = in.value("direction_min", -k_pi * 0.25f);
    direction_max = in.value("direction_max",  k_pi * 0.25f);
    auto acc = in.value("acceleration", std::vector<float>{0.0f, 0.0f});
    if (acc.size() >= 2) acceleration = math::Vector2f(acc[0], acc[1]);
    auto sc = in.value("start_color", std::vector<float>{1, 1, 1, 1});
    if (sc.size() >= 4) start_color = render::Color(sc[0], sc[1], sc[2], sc[3]);
    auto ec = in.value("end_color", std::vector<float>{1, 1, 1, 0});
    if (ec.size() >= 4) end_color = render::Color(ec[0], ec[1], ec[2], ec[3]);
    start_size = in.value("start_size", 8.0f);
    end_size = in.value("end_size", 2.0f);
    rotation_min = in.value("rotation_min", 0.0f);
    rotation_max = in.value("rotation_max", 0.0f);
    angular_velocity_min = in.value("angular_velocity_min", -180.0f);
    angular_velocity_max = in.value("angular_velocity_max", 180.0f);
    texture_path = in.value("texture_path", std::string());
    additive = in.value("additive", false);
    auto eo = in.value("emission_offset", std::vector<float>{0.0f, 0.0f});
    if (eo.size() >= 2) emission_offset = math::Vector2f(eo[0], eo[1]);
}

} // namespace gryce_engine::components::d2
