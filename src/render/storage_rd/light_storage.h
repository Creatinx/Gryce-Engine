#pragma once

#include <cstdint>
#include <vector>

#include "render/export.h"
#include "render/rendering_server.h"
#include "math/math.h"

namespace gryce_engine::render {

// 光源数据（类似 Godot 的 LightData）
struct LightData {
    LightType type = LightType::Directional;
    math::Vector3f position = math::Vector3f::zero();
    math::Vector3f direction = math::Vector3f(0.0f, -1.0f, 0.0f);
    math::Vector3f color = math::Vector3f::one();
    float intensity = 1.0f;
    float range = 10.0f;
    float spot_angle = 45.0f;
    float spot_softness = 0.2f;
    bool shadow_enabled = true;
    uint32_t shadow_map_id = 0;
};

// 光源 RID 句柄
using LightRID = uint32_t;
constexpr LightRID k_invalid_light_rid = 0;

// 方向光数据（类似 Godot 的 DirectionalLightData）
struct DirectionalLightData {
    math::Vector3f direction;
    math::Vector3f color;
    float intensity;
    float shadow_opacity;
    float specular;
    uint32_t mask;
    uint32_t bake_mode;
    float shadow_bias;
    float shadow_normal_bias;
};

// ---------------------------------------------------------------------------
// RendererLightStorage — 光源存储抽象接口
// 管理所有光源的创建、更新、销毁，维护光源 GPU 数据缓冲。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererLightStorage {
public:
    virtual ~RendererLightStorage() = default;

    virtual LightRID light_create(LightType type) = 0;
    virtual void light_free(LightRID rid) = 0;
    virtual void light_set_color(LightRID rid, const math::Vector3f& color) = 0;
    virtual void light_set_intensity(LightRID rid, float intensity) = 0;
    virtual void light_set_position(LightRID rid, const math::Vector3f& pos) = 0;
    virtual void light_set_direction(LightRID rid, const math::Vector3f& dir) = 0;
    virtual void light_set_range(LightRID rid, float range) = 0;
    virtual void light_set_spot_angle(LightRID rid, float angle) = 0;
    virtual void light_set_shadow_enabled(LightRID rid, bool enabled) = 0;

    virtual const LightData* get_light_data(LightRID rid) const = 0;
    virtual size_t light_count() const = 0;
    virtual void update_buffers() = 0;
};

} // namespace gryce_engine::render