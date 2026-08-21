#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>

#include "render/export.h"
#include "math/math.h"
#include "math/camera.h"

namespace gryce_engine {
namespace scene { class Scene; }
} // namespace gryce_engine

namespace gryce_engine::render {

class RenderContext;
class RendererCompositor;

// 光源类型（与 storage_rd 一致）
enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2
};

// 光源参数
enum class LightParam : uint8_t {
    Energy = 0,
    Range,
    SpotAngle,
    SpotSoftness,
    ShadowOpacity,
    ShadowBias,
    ShadowNormalBias,
    MAX
};

// ---------------------------------------------------------------------------
// RenderingServer — 线程安全渲染 API 层
// 类似 Godot 的 RenderingServer，所有 API 调用通过命令队列异步发送到渲染线程。
// 当前实现为同步包装（后续可加命令队列）。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderingServer {
public:
    virtual ~RenderingServer() = default;

    // --- 单例 ---
    static RenderingServer* get_singleton();
    static RenderingServer* create(RendererCompositor* compositor);

    // --- 场景管理 ---
    virtual uint32_t scene_create() = 0;
    virtual void scene_free(uint32_t scene_id) = 0;
    virtual void scene_add_node(uint32_t scene_id, uint32_t node_id) = 0;
    virtual void scene_remove_node(uint32_t scene_id, uint32_t node_id) = 0;

    // --- 实体管理 ---
    virtual uint32_t entity_create() = 0;
    virtual void entity_free(uint32_t entity_id) = 0;
    virtual void entity_set_transform(uint32_t entity_id, const math::Matrix4f& transform) = 0;
    virtual void entity_set_mesh(uint32_t entity_id, uint32_t mesh_id) = 0;
    virtual void entity_set_material(uint32_t entity_id, uint32_t material_id) = 0;
    virtual void entity_set_visible(uint32_t entity_id, bool visible) = 0;

    // --- 光源管理 ---
    virtual uint32_t light_create(LightType type) = 0;
    virtual void light_free(uint32_t light_id) = 0;
    virtual void light_set_color(uint32_t light_id, const math::Vector3f& color) = 0;
    virtual void light_set_param(uint32_t light_id, LightParam param, float value) = 0;
    virtual void light_set_transform(uint32_t light_id, const math::Matrix4f& transform) = 0;

    // --- 渲染控制 ---
    virtual void set_viewport(int w, int h) = 0;
    virtual void set_camera(const math::Camera& camera) = 0;
    virtual void set_scene(class scene::Scene* scene) = 0;
    virtual void render_frame() = 0;

    // --- 材质/网格/纹理 ---
    virtual uint32_t mesh_create() = 0;
    virtual void mesh_free(uint32_t mesh_id) = 0;
    virtual uint32_t material_create() = 0;
    virtual void material_free(uint32_t material_id) = 0;
    virtual uint32_t texture_create() = 0;
    virtual void texture_free(uint32_t texture_id) = 0;

    // --- 资源加载 ---
    virtual uint32_t load_mesh(const std::string& path) = 0;
    virtual uint32_t load_texture(const std::string& path) = 0;
    virtual uint32_t load_material(const std::string& path) = 0;
};

} // namespace gryce_engine::render