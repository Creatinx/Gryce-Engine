#pragma once

#include <cstdint>
#include <vector>

#include "render/export.h"
#include "math/math.h"

namespace gryce_engine::render {

// 粒子 RID 句柄
using ParticleRID = uint32_t;
constexpr ParticleRID k_invalid_particle_rid = 0;

// 粒子数据
struct ParticleData {
    math::Vector3f position;
    math::Vector3f velocity;
    math::Vector4f color = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    float size = 1.0f;
    float lifetime = 1.0f;
    float age = 0.0f;
    uint32_t flags = 0;
};

// ---------------------------------------------------------------------------
// RendererParticlesStorage — 粒子存储抽象接口
// 管理粒子系统的 GPU 数据缓冲。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererParticlesStorage {
public:
    virtual ~RendererParticlesStorage() = default;

    virtual ParticleRID particles_create() = 0;
    virtual void particles_free(ParticleRID rid) = 0;
    virtual void particles_set_amount(ParticleRID rid, uint32_t amount) = 0;
    virtual void particles_set_emitting(ParticleRID rid, bool emitting) = 0;
    virtual void particles_set_data(ParticleRID rid, const std::vector<ParticleData>& data) = 0;

    virtual void update_buffers() = 0;
};

} // namespace gryce_engine::render