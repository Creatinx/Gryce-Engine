#include "render/rendering_server_default.h"
#include "render/render_context.h"
#include "render/renderer_scene_render.h"
#include "scene/scene.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "assets/texture_data.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 单例
static RenderingServer* s_singleton = nullptr;

RenderingServer* RenderingServer::get_singleton() {
    return s_singleton;
}

RenderingServer* RenderingServer::create(RendererCompositor* compositor) {
    if (!s_singleton) {
        s_singleton = new RenderingServerDefault(compositor);
    }
    return s_singleton;
}

// ---------------------------------------------------------------------------
// RenderingServerDefault
// ---------------------------------------------------------------------------

RenderingServerDefault::RenderingServerDefault(RendererCompositor* compositor)
    : compositor_(compositor) {
    // 获取 RenderContext
    if (compositor_ && compositor_->get_scene()) {
        // ctx_ = compositor_->get_scene()->get_context();
    }

    // 初始化 Storage 系统
    mesh_storage_ = std::make_unique<MeshStorageImpl>(ctx_);
    material_storage_ = std::make_unique<MaterialStorageImpl>();
    texture_storage_ = std::make_unique<TextureStorageImpl>(ctx_);
    light_storage_ = std::make_unique<LightStorageImpl>(ctx_);

    GLOG_INFO("RenderingServerDefault initialized");
}

RenderingServerDefault::~RenderingServerDefault() {
    s_singleton = nullptr;
}

// 场景管理
uint32_t RenderingServerDefault::scene_create() {
    uint32_t id = next_scene_id_++;
    scenes_[id] = SceneData{};
    return id;
}

void RenderingServerDefault::scene_free(uint32_t scene_id) {
    scenes_.erase(scene_id);
}

void RenderingServerDefault::scene_add_node(uint32_t scene_id, uint32_t node_id) {
    auto it = scenes_.find(scene_id);
    if (it != scenes_.end()) {
        it->second.nodes.push_back(node_id);
    }
}

void RenderingServerDefault::scene_remove_node(uint32_t scene_id, uint32_t node_id) {
    auto it = scenes_.find(scene_id);
    if (it != scenes_.end()) {
        auto& nodes = it->second.nodes;
        nodes.erase(std::remove(nodes.begin(), nodes.end(), node_id), nodes.end());
    }
}

// 实体管理
uint32_t RenderingServerDefault::entity_create() {
    uint32_t id = next_entity_id_++;
    entities_[id] = EntityData{};
    return id;
}

void RenderingServerDefault::entity_free(uint32_t entity_id) {
    entities_.erase(entity_id);
}

void RenderingServerDefault::entity_set_transform(uint32_t entity_id, const math::Matrix4f& transform) {
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) it->second.transform = transform;
}

void RenderingServerDefault::entity_set_mesh(uint32_t entity_id, uint32_t mesh_id) {
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) it->second.mesh_id = mesh_id;
}

void RenderingServerDefault::entity_set_material(uint32_t entity_id, uint32_t material_id) {
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) it->second.material_id = material_id;
}

void RenderingServerDefault::entity_set_visible(uint32_t entity_id, bool visible) {
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) it->second.visible = visible;
}

// 光源管理
uint32_t RenderingServerDefault::light_create(LightType type) {
    uint32_t id = next_light_id_++;
    lights_[id] = LightInstance{};
    lights_[id].type = type;
    return id;
}

void RenderingServerDefault::light_free(uint32_t light_id) {
    lights_.erase(light_id);
}

void RenderingServerDefault::light_set_color(uint32_t light_id, const math::Vector3f& color) {
    auto it = lights_.find(light_id);
    if (it != lights_.end()) it->second.color = color;
}

void RenderingServerDefault::light_set_param(uint32_t light_id, LightParam param, float value) {
    auto it = lights_.find(light_id);
    if (it == lights_.end()) return;
    switch (param) {
        case LightParam::Energy: it->second.intensity = value; break;
        case LightParam::Range: it->second.range = value; break;
        case LightParam::SpotAngle: it->second.spot_angle = value; break;
        case LightParam::SpotSoftness: it->second.spot_softness = value; break;
        case LightParam::ShadowOpacity: it->second.shadow_opacity = value; break;
        case LightParam::ShadowBias: it->second.shadow_bias = value; break;
        case LightParam::ShadowNormalBias: it->second.shadow_normal_bias = value; break;
        default: break;
    }
}

void RenderingServerDefault::light_set_transform(uint32_t light_id, const math::Matrix4f& transform) {
    auto it = lights_.find(light_id);
    if (it == lights_.end()) return;
    // 从变换矩阵提取位置和方向
    it->second.position = math::Vector3f(transform(3, 0), transform(3, 1), transform(3, 2));
    // 方向 = Z 轴负方向（默认朝前）
    it->second.direction = math::Vector3f(-transform(2, 0), -transform(2, 1), -transform(2, 2));
    it->second.direction = it->second.direction.normalized();
}

// 渲染控制
void RenderingServerDefault::set_viewport(int w, int h) {
    viewport_w_ = w;
    viewport_h_ = h;
}

void RenderingServerDefault::set_camera(const math::Camera& camera) {
    current_camera_ = camera;
}

void RenderingServerDefault::set_scene(scene::Scene* scene) {
    scene_ = scene;
}

void RenderingServerDefault::render_frame() {
    if (!compositor_ || !compositor_->get_scene()) return;

    // 构建 RenderData
    RendererSceneRender::RenderData data;
    data.scene = scene_;
    data.camera = &current_camera_;
    data.viewport_width = viewport_w_;
    data.viewport_height = viewport_h_;

    // 渲染场景
    compositor_->get_scene()->render_scene(data);
}

// 材质/网格/纹理
uint32_t RenderingServerDefault::mesh_create() {
    return mesh_storage_->mesh_create();
}

void RenderingServerDefault::mesh_free(uint32_t mesh_id) {
    mesh_storage_->mesh_free(mesh_id);
}

uint32_t RenderingServerDefault::material_create() {
    return material_storage_->material_create();
}

void RenderingServerDefault::material_free(uint32_t material_id) {
    material_storage_->material_free(material_id);
}

uint32_t RenderingServerDefault::texture_create() {
    return texture_storage_->texture_create();
}

void RenderingServerDefault::texture_free(uint32_t texture_id) {
    texture_storage_->texture_free(texture_id);
}

// 资源加载
uint32_t RenderingServerDefault::load_mesh(const std::string& path) {
    // 通过 AssetManager 加载网格数据
    auto mesh_data = assets::AssetManager::instance().load_mesh(path);
    if (!mesh_data) {
        GLOG_ERROR("RenderingServerDefault::load_mesh: failed to load '{}'", path);
        return 0;
    }

    // 创建网格 RID
    uint32_t rid = mesh_storage_->mesh_create();

    // 转换顶点数据（assets::MeshVertex → render::MeshVertex）
    std::vector<MeshVertex> vertices;
    vertices.reserve(mesh_data->vertices.size());
    for (const auto& src_v : mesh_data->vertices) {
        MeshVertex v;
        v.position = src_v.position;
        v.normal = src_v.normal;
        v.tangent = src_v.tangent;
        v.uv = src_v.uv;
        vertices.push_back(v);
    }
    mesh_storage_->mesh_set_vertices(rid, vertices);
    mesh_storage_->mesh_set_indices(rid, mesh_data->indices);

    // 设置子网格（MeshData 暂不支持子网格，后续可扩展）

    GLOG_INFO("RenderingServerDefault::load_mesh: loaded '{}' (rid={}, {} verts, {} idxs)",
              path, rid, vertices.size(), mesh_data->indices.size());
    return rid;
}

uint32_t RenderingServerDefault::load_texture(const std::string& path) {
    // 加载纹理数据
    auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(path);
    if (!tex_data) {
        GLOG_ERROR("RenderingServerDefault::load_texture: failed to load '{}'", path);
        return 0;
    }

    // 创建纹理 RID
    uint32_t rid = texture_storage_->texture_create();
    texture_storage_->texture_set_path(rid, path);

    // 转换格式并设置数据
    TextureType tex_type = TextureType::Texture2D;
    TextureFormat fmt = TextureFormat::RGBA8;
    // 根据纹理数据通道数选择格式
    switch (tex_data->channels) {
        case 1: fmt = TextureFormat::R8; break;
        case 2: fmt = TextureFormat::RG8; break;
        case 3: fmt = TextureFormat::RGB8; break;
        default: fmt = TextureFormat::RGBA8; break;
    }

    texture_storage_->texture_set_data(rid, tex_type, fmt,
        tex_data->width, tex_data->height, 1, tex_data->mip_levels,
        tex_data->pixels.data());

    GLOG_INFO("RenderingServerDefault::load_texture: loaded '{}' (rid={}, {}x{})",
              path, rid, tex_data->width, tex_data->height);
    return rid;
}

uint32_t RenderingServerDefault::load_material(const std::string& path) {
    // 创建材质 RID
    uint32_t rid = material_storage_->material_create();

    // 设置默认材质参数
    material_storage_->material_set_param(rid, "albedo", math::Vector3f(1.0f, 1.0f, 1.0f));
    material_storage_->material_set_param(rid, "roughness", 0.5f);
    material_storage_->material_set_param(rid, "metallic", 0.0f);

    GLOG_INFO("RenderingServerDefault::load_material: created '{}' (rid={})", path, rid);
    return rid;
}

} // namespace gryce_engine::render