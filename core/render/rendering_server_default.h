#pragma once

#include "render/rendering_server.h"
#include "render/renderer_compositor.h"
#include "render/renderer_scene_render.h"
#include "render/storage_rd/mesh_storage_impl.h"
#include "render/storage_rd/material_storage_impl.h"
#include "render/storage_rd/texture_storage_impl.h"
#include "render/storage_rd/light_storage_impl.h"
#include "math/camera.h"

#include <unordered_map>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RenderingServerDefault — 默认 RenderingServer 实现
// 同步包装 RendererCompositor 的 API。
// ---------------------------------------------------------------------------
class RenderingServerDefault : public RenderingServer {
public:
    RenderingServerDefault(RendererCompositor* compositor);
    ~RenderingServerDefault() override;

    // 场景管理
    uint32_t scene_create() override;
    void scene_free(uint32_t scene_id) override;
    void scene_add_node(uint32_t scene_id, uint32_t node_id) override;
    void scene_remove_node(uint32_t scene_id, uint32_t node_id) override;

    // 实体管理
    uint32_t entity_create() override;
    void entity_free(uint32_t entity_id) override;
    void entity_set_transform(uint32_t entity_id, const math::Matrix4f& transform) override;
    void entity_set_mesh(uint32_t entity_id, uint32_t mesh_id) override;
    void entity_set_material(uint32_t entity_id, uint32_t material_id) override;
    void entity_set_visible(uint32_t entity_id, bool visible) override;

    // 光源管理
    uint32_t light_create(LightType type) override;
    void light_free(uint32_t light_id) override;
    void light_set_color(uint32_t light_id, const math::Vector3f& color) override;
    void light_set_param(uint32_t light_id, LightParam param, float value) override;
    void light_set_transform(uint32_t light_id, const math::Matrix4f& transform) override;

    // 渲染控制
    void set_viewport(int w, int h) override;
    void set_camera(const math::Camera& camera) override;
    void set_scene(class scene::Scene* scene) override;
    void render_frame() override;

    // 材质/网格/纹理
    uint32_t mesh_create() override;
    void mesh_free(uint32_t mesh_id) override;
    uint32_t material_create() override;
    void material_free(uint32_t material_id) override;
    uint32_t texture_create() override;
    void texture_free(uint32_t texture_id) override;

    // 资源加载
    uint32_t load_mesh(const std::string& path) override;
    uint32_t load_texture(const std::string& path) override;
    uint32_t load_material(const std::string& path) override;

private:
    struct EntityData {
        math::Matrix4f transform;
        uint32_t mesh_id = 0;
        uint32_t material_id = 0;
        bool visible = true;
    };

    struct LightInstance {
        LightType type;
        math::Vector3f position;
        math::Vector3f direction;
        math::Vector3f color;
        float intensity = 1.0f;
        float range = 10.0f;
        float spot_angle = 45.0f;
        float spot_softness = 0.2f;
        float shadow_opacity = 1.0f;
        float shadow_bias = 0.1f;
        float shadow_normal_bias = 0.1f;
    };

    struct SceneData {
        std::vector<uint32_t> nodes;
    };

    RendererCompositor* compositor_ = nullptr;
    RenderContext* ctx_ = nullptr;

    // Storage 系统
    std::unique_ptr<MeshStorageImpl> mesh_storage_;
    std::unique_ptr<MaterialStorageImpl> material_storage_;
    std::unique_ptr<TextureStorageImpl> texture_storage_;
    std::unique_ptr<LightStorageImpl> light_storage_;

    // 场景/实体/光源
    std::unordered_map<uint32_t, SceneData> scenes_;
    std::unordered_map<uint32_t, EntityData> entities_;
    std::unordered_map<uint32_t, LightInstance> lights_;

    // 当前相机和视口
    math::Camera current_camera_;
    int viewport_w_ = 1280;
    int viewport_h_ = 720;
    scene::Scene* scene_ = nullptr;

    // RID 分配
    uint32_t next_scene_id_ = 1;
    uint32_t next_entity_id_ = 1;
    uint32_t next_light_id_ = 1;
    uint32_t next_mesh_id_ = 1;
    uint32_t next_material_id_ = 1;
    uint32_t next_texture_id_ = 1;
};

} // namespace gryce_engine::render