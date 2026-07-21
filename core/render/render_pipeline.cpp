#include "render_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "render/render_context.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "ecs/query.h"
#include "math/camera.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

RenderPipeline::RenderPipeline() = default;

RenderPipeline::~RenderPipeline() {
    shutdown();
}

bool RenderPipeline::init(RenderContext* ctx, const std::string& shader_dir) {
    if (!ctx) return false;
    ctx_ = ctx;
    shader_dir_ = resources::ResourcePath::resolve(shader_dir);

    if (!create_shadow_map(ctx)) {
        GLOG_ERROR("RenderPipeline: failed to create shadow map");
        return false;
    }

    if (hdr_enabled_) {
        if (!create_hdr_target(ctx)) {
            GLOG_WARN("RenderPipeline: HDR target failed, falling back to LDR");
            hdr_enabled_ = false;
        }
    }

    // 编辑器视口离屏输出依赖 HDR 管线（tonemap 结果即 LDR 视口纹理）
    if (hdr_enabled_ && viewport_output_enabled_) {
        if (!create_viewport_target(ctx)) {
            GLOG_WARN("RenderPipeline: viewport output target failed, disabling");
            viewport_output_enabled_ = false;
        }
    }

    pbr_shader_ = load_shader("pbr", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!pbr_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load pbr shader");
        return false;
    }

    // 蒙皮 PBR shader 为可选项：旧项目 shader 目录没有 skinned_pbr 时
    // 只告警降级（SkinnedMeshRenderer 不绘制），不影响普通渲染管线。
    skinned_pbr_shader_ = load_shader("skinned_pbr", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false, true);
    if (!skinned_pbr_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: skinned_pbr shader unavailable, skinned rendering disabled");
    }

    // Scene View 网格线 shader：可选，加载失败仅禁用网格线。
    grid_shader_ = load_shader("grid", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!grid_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: grid shader unavailable, viewport grid disabled");
    } else if (!create_grid_mesh(ctx)) {
        GLOG_WARN("RenderPipeline: grid mesh creation failed, viewport grid disabled");
        grid_shader_ = RHIShaderHandle{};
    }

    shadow_shader_ = load_shader("shadow_map", shadow_fbo_, false, false);
    if (!shadow_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load shadow shader");
        return false;
    }

    tonemap_shader_ = load_shader("tonemap", RHIFramebufferHandle{}, true, true);
    if (!tonemap_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load tonemap shader");
        return false;
    }

    // 预先把 shadow sampler 绑定到固定 PBR shadow slot，避免首次使用 PBR shader
    // 时 uShadowMap 默认指向 texture unit 0（默认纹理非 depth），触发 NVIDIA undefined
    // behavior warning。注意 set_uniform_int 前必须先 set_shader，否则 GL 报无 active program。
    if (shadow_map_.is_valid()) {
        auto bind_shadow_sampler = [this](RHIShaderHandle shader) {
            if (!shader.is_valid()) return;
            ctx_->set_shader(shader);
            ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
            if (shadow_map_ptr) {
                shadow_map_ptr->bind(TextureSlots::kPBRShadow);
            }
            ctx_->set_texture(shader, shadow_map_, TextureSlots::kPBRShadow, "");
            ctx_->set_uniform_int(shader, "uShadowMap", TextureSlots::kPBRShadow);
        };
        bind_shadow_sampler(pbr_shader_);
        bind_shadow_sampler(skinned_pbr_shader_);
    }

    if (hdr_enabled_) {
        if (!create_fullscreen_mesh(ctx)) {
            GLOG_WARN("RenderPipeline: fullscreen mesh failed, falling back to LDR");
            hdr_enabled_ = false;
        }
    }

    initialized_ = true;
    GLOG_INFO("RenderPipeline initialized (PBR + multi-light + shadow + {}{})",
              hdr_enabled_ ? "HDR" : "LDR", " + skybox-ready");
    return true;
}

void RenderPipeline::shutdown() {
    if (!ctx_) return;

    clear_skybox();

    if (fullscreen_mesh_.is_valid()) {
        ctx_->destroy_mesh(fullscreen_mesh_);
        fullscreen_mesh_ = RHIMeshHandle{};
    }
    if (hdr_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(hdr_fbo_);
        hdr_fbo_ = RHIFramebufferHandle{};
    }
    if (hdr_color_.is_valid()) {
        ctx_->destroy_texture(hdr_color_);
        hdr_color_ = RHITextureHandle{};
    }
    if (hdr_depth_.is_valid()) {
        ctx_->destroy_texture(hdr_depth_);
        hdr_depth_ = RHITextureHandle{};
    }
    if (viewport_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(viewport_fbo_);
        viewport_fbo_ = RHIFramebufferHandle{};
    }
    if (viewport_color_.is_valid()) {
        ctx_->destroy_texture(viewport_color_);
        viewport_color_ = RHITextureHandle{};
    }
    if (owns_shaders_ && tonemap_shader_.is_valid()) {
        ctx_->destroy_shader(tonemap_shader_);
        tonemap_shader_ = RHIShaderHandle{};
    }

    if (grid_mesh_.is_valid()) {
        ctx_->destroy_mesh(grid_mesh_);
        grid_mesh_ = RHIMeshHandle{};
    }
    if (owns_shaders_ && grid_shader_.is_valid()) {
        ctx_->destroy_shader(grid_shader_);
        grid_shader_ = RHIShaderHandle{};
    }

    if (shadow_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(shadow_fbo_);
        shadow_fbo_ = RHIFramebufferHandle{};
    }
    if (shadow_map_.is_valid()) {
        ctx_->destroy_texture(shadow_map_);
        shadow_map_ = RHITextureHandle{};
    }
    if (owns_shaders_) {
        if (pbr_shader_.is_valid()) ctx_->destroy_shader(pbr_shader_);
        if (shadow_shader_.is_valid()) ctx_->destroy_shader(shadow_shader_);
        if (skinned_pbr_shader_.is_valid()) ctx_->destroy_shader(skinned_pbr_shader_);
    }
    pbr_shader_ = RHIShaderHandle{};
    shadow_shader_ = RHIShaderHandle{};
    skinned_pbr_shader_ = RHIShaderHandle{};
    ctx_ = nullptr;
    initialized_ = false;
}

RHIShaderHandle RenderPipeline::load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process,
                                            bool skinned) {
    RHIShaderHandle shader = ctx_->create_shader();
    IShader* shader_ptr = ctx_->shader(shader);
    IFramebuffer* target_ptr = ctx_->framebuffer(target);
    if (!shader.is_valid() || !shader_ptr || !shader_ptr->load_program(name, shader_dir_, target_ptr, color_output, post_process, false, skinned)) {
        GLOG_ERROR("RenderPipeline: failed to load shader program '{}'", name);
        if (shader.is_valid()) {
            ctx_->destroy_shader(shader);
        }
        return RHIShaderHandle{};
    }
    owns_shaders_ = true;
    return shader;
}

bool RenderPipeline::create_shadow_map(RenderContext* ctx) {
    shadow_map_ = ctx->create_texture();
    ITexture* shadow_map_ptr = ctx->texture(shadow_map_);
    if (!shadow_map_.is_valid() || !shadow_map_ptr || !shadow_map_ptr->create_depth(shadow_map_size_, shadow_map_size_)) {
        return false;
    }
    shadow_map_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    shadow_map_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    shadow_fbo_ = ctx->create_framebuffer();
    IFramebuffer* shadow_fbo_ptr = ctx->framebuffer(shadow_fbo_);
    if (!shadow_fbo_.is_valid() || !shadow_fbo_ptr || !shadow_fbo_ptr->create(shadow_map_size_, shadow_map_size_)) {
        return false;
    }
    shadow_fbo_ptr->attach_depth_texture(shadow_map_ptr);
    if (!shadow_fbo_ptr->is_complete()) {
        GLOG_ERROR("RenderPipeline: shadow framebuffer incomplete");
        return false;
    }
    return true;
}

void RenderPipeline::set_camera(const math::Camera& camera) {
    camera_ = const_cast<math::Camera*>(&camera);
}

void RenderPipeline::set_lights(const std::vector<Light>& lights) {
    lights_ = lights;
    if (lights_.size() > static_cast<size_t>(k_max_lights)) {
        lights_.resize(k_max_lights);
    }
}

void RenderPipeline::set_viewport(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
}

ITexture* RenderPipeline::shadow_map() const {
    return ctx_ ? ctx_->texture(shadow_map_) : nullptr;
}

void RenderPipeline::update_light_space_matrix() {
    // 阴影只由第一个方向光投射
    shadow_light_index_ = -1;
    for (size_t i = 0; i < lights_.size(); ++i) {
        if (lights_[i].type == LightType::Directional) {
            shadow_light_index_ = static_cast<int>(i);
            break;
        }
    }
    if (shadow_light_index_ < 0) {
        light_space_matrix_ = math::Matrix4f::identity();
        return;
    }

    // 阴影正交盒跟随相机焦点（相机前方 shadow_area_ 处），避免远处物体出盒失影。
    // 方向光方向为零向量时 normalized() 产生 NaN，look_at 会崩坏；先判零再归一化。
    const math::Vector3f& raw_dir = lights_[shadow_light_index_].direction;
    math::Vector3f light_dir = raw_dir.length_sq() < 1e-6f
                               ? math::Vector3f(0.0f, -1.0f, 0.0f)
                               : raw_dir.normalized();
    math::Vector3f center = math::Vector3f::zero();
    if (camera_) {
        center = camera_->position() + camera_->forward() * shadow_area_;
    }
    math::Vector3f eye = center - light_dir * (shadow_area_ * 2.0f);
    math::Matrix4f light_view = math::Matrix4f::look_at(eye, center, math::Vector3f::up());
    math::Matrix4f light_proj = math::Matrix4f::ortho(
        -shadow_area_, shadow_area_, -shadow_area_, shadow_area_, 0.1f, shadow_area_ * 4.0f);
    light_space_matrix_ = light_proj * light_view;
}

// ---------------------------------------------------------------------------
// Skybox
// ---------------------------------------------------------------------------
bool RenderPipeline::set_skybox(const std::array<std::string, 6>& face_paths) {
    if (!ctx_) return false;

    clear_skybox();

    // 1. 加载六个面的 CPU 数据（全部要求同尺寸同通道）
    const assets::TextureData* faces[6] = {};
    for (int i = 0; i < 6; ++i) {
        auto handle = assets::AssetManager::instance().load<assets::TextureData>(face_paths[i]);
        if (!handle.valid()) {
            GLOG_ERROR("RenderPipeline: failed to load skybox face '{}'", face_paths[i]);
            return false;
        }
        faces[i] = handle.get();
        if (i > 0 && (faces[i]->width != faces[0]->width || faces[i]->height != faces[0]->height ||
                      faces[i]->channels != faces[0]->channels)) {
            GLOG_ERROR("RenderPipeline: skybox faces must have identical size/channels");
            return false;
        }
    }

    // 2. 上传 cubemap（必须在 RenderContext::start() 之前，主线程持有 GPU context）
    skybox_texture_ = ctx_->create_texture();
    ITexture* tex_ptr = ctx_->texture(skybox_texture_);
    if (!skybox_texture_.is_valid() || !tex_ptr) return false;
    const void* face_data[6] = {};
    for (int i = 0; i < 6; ++i) face_data[i] = faces[i]->data();
    if (!tex_ptr->upload_cubemap(face_data, faces[0]->width, faces[0]->height, faces[0]->channels)) {
        GLOG_ERROR("RenderPipeline: failed to upload skybox cubemap");
        clear_skybox();
        return false;
    }

    // 3. 加载 skybox shader（Vulkan 走专用管线变体）
    {
        RHIShaderHandle shader = ctx_->create_shader();
        IShader* shader_ptr = ctx_->shader(shader);
        IFramebuffer* target_ptr = hdr_enabled_ ? ctx_->framebuffer(hdr_fbo_) : nullptr;
        if (!shader.is_valid() || !shader_ptr ||
            !shader_ptr->load_program("skybox", shader_dir_, target_ptr, true, false, true)) {
            GLOG_ERROR("RenderPipeline: failed to load skybox shader");
            clear_skybox();
            return false;
        }
        skybox_shader_ = shader;
        owns_shaders_ = true;
    }

    if (!create_skybox_mesh(ctx_)) {
        clear_skybox();
        return false;
    }

    GLOG_INFO("RenderPipeline: skybox set ({} faces, {}x{})", 6, faces[0]->width, faces[0]->height);
    return true;
}

void RenderPipeline::clear_skybox() {
    if (!ctx_) return;
    if (skybox_mesh_.is_valid()) {
        ctx_->destroy_mesh(skybox_mesh_);
        skybox_mesh_ = RHIMeshHandle{};
    }
    if (skybox_shader_.is_valid()) {
        ctx_->destroy_shader(skybox_shader_);
        skybox_shader_ = RHIShaderHandle{};
    }
    if (skybox_texture_.is_valid()) {
        ctx_->destroy_texture(skybox_texture_);
        skybox_texture_ = RHITextureHandle{};
    }
}

bool RenderPipeline::create_skybox_mesh(RenderContext* ctx) {
    // 单位立方体（36 顶点），顶点布局与 MeshRenderer 一致（56 字节），
    // 只有 position 有意义，skybox shader 采样方向即顶点坐标。
    struct VertexGPU {
        float x, y, z;
        float nx, ny, nz;
        float tx, ty, tz;
        float u, v;
        float r, g, b;
    };
    const float p = 1.0f;
    const float n = -1.0f;
    const math::Vector3f positions[36] = {
        {n, n, p}, {p, p, p}, {p, n, p}, {p, p, p}, {n, n, p}, {n, p, p}, // front
        {p, n, n}, {n, p, n}, {n, n, n}, {n, p, n}, {p, n, n}, {p, p, n}, // back
        {n, n, n}, {n, n, p}, {n, p, p}, {n, p, p}, {n, p, n}, {n, n, n}, // left
        {p, n, p}, {p, n, n}, {p, p, n}, {p, p, n}, {p, p, p}, {p, n, p}, // right
        {n, p, p}, {p, p, p}, {p, p, n}, {p, p, n}, {n, p, n}, {n, p, p}, // top
        {n, n, n}, {p, n, n}, {p, n, p}, {p, n, p}, {n, n, p}, {n, n, n}, // bottom
    };

    std::vector<VertexGPU> verts(36);
    for (int i = 0; i < 36; ++i) {
        verts[i] = {positions[i].x, positions[i].y, positions[i].z,
                    0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1};
    }

    skybox_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(skybox_mesh_);
    if (!skybox_mesh_.is_valid() || !mesh_ptr) return false;
    mesh_ptr->upload_vertices(verts.data(),
                              static_cast<uint32_t>(verts.size() * sizeof(VertexGPU)),
                              static_cast<uint32_t>(verts.size()));
    VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::render_skybox(RenderContext& ctx) {
    if (!skybox_texture_.is_valid() || !skybox_shader_.is_valid() || !skybox_mesh_.is_valid() || !camera_) {
        return;
    }

    // 天空盒：关深度测试/写入、关剔除（从立方体内部观察），画完后恢复
    ctx.set_depth_test(false);
    ctx.set_depth_write(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    // 去掉 view 的平移分量，让天空盒始终跟随相机
    math::Matrix4f view = camera_->get_view_matrix();
    view(0, 3) = 0.0f;
    view(1, 3) = 0.0f;
    view(2, 3) = 0.0f;

    ctx.set_shader(skybox_shader_);
    ctx.set_uniform_mat4(skybox_shader_, "uView", view);
    ctx.set_uniform_mat4(skybox_shader_, "uProjection", camera_->get_projection_matrix());

    ITexture* tex_ptr = ctx_->texture(skybox_texture_);
    if (tex_ptr) {
        tex_ptr->bind(TextureSlots::kSkyboxCube);
    }
    ctx.set_texture(skybox_shader_, skybox_texture_, TextureSlots::kSkyboxCube, "");
    ctx.set_uniform_int(skybox_shader_, "uSkybox", TextureSlots::kSkyboxCube);

    ctx.draw_mesh(skybox_mesh_, skybox_shader_);

    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(!cull_disabled_);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void RenderPipeline::render_scene(scene::Scene& scene, RenderContext& ctx) {
    if (!initialized_ || !camera_) return;

    update_light_space_matrix();
    const bool render_shadow = shadow_enabled_ && shadow_light_index_ >= 0;

    // 1. Shadow pass（仅第一个方向光）
    if (render_shadow) {
        begin_shadow_pass(ctx);
        ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
            scene,
            [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
                if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
                ctx.set_uniform_mat4(shadow_shader_, "uModel", entity->world_transform());
                ctx.set_uniform_mat4(shadow_shader_, "uLightSpaceMatrix", light_space_matrix_);
                ctx.draw_mesh(mr->gpu_mesh_handle(), shadow_shader_);
            });
        end_shadow_pass(ctx);
    }

    // 2. Forward PBR pass (HDR target or backbuffer)
    if (hdr_enabled_) {
        begin_hdr_forward_pass(ctx);
    } else {
        begin_forward_pass(ctx);
    }

    // 2a. Skybox（最先绘制，作为背景）
    render_skybox(ctx);

    // 2a'. Scene View 网格线（skybox 之后，场景物体之前）
    render_grid(ctx);

    bind_global_uniforms(ctx);

    // 2b. 收集绘制项并拆分不透明 / 透明
    struct DrawItem {
        RHIMeshHandle mesh;
        const Material* material;
        math::Matrix4f model;
        float dist_sq;
    };
    struct SkinnedDrawItem {
        RHIMeshHandle mesh;
        const Material* material;
        math::Matrix4f model;
        std::shared_ptr<const std::vector<math::Matrix4f>> palette;
        float dist_sq;
    };
    std::vector<DrawItem> opaque_items;
    std::vector<DrawItem> transparent_items;
    std::vector<SkinnedDrawItem> skinned_opaque_items;
    std::vector<SkinnedDrawItem> skinned_transparent_items;
    const math::Vector3f cam_pos = camera_->position();
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
            const math::Matrix4f& model = entity->world_transform();
            const Material* mat = mr->material.get();
            const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
            math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
            float dist_sq = (pos - cam_pos).length_sq();
            DrawItem item{mr->gpu_mesh_handle(), mat, model, dist_sq};
            if (transparent) {
                transparent_items.push_back(item);
            } else {
                opaque_items.push_back(item);
            }
        });

    // 蒙皮网格：skinned 管线可用且 palette 已就绪才绘制
    if (skinned_pbr_shader_.is_valid()) {
        ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
            scene,
            [&](scene::Entity* entity, components::SkinnedMeshRenderer* mr, components::Transform* /*transform*/) {
                if (!mr->enabled || !mr->gpu_mesh_handle().is_valid() || !mr->palette()) return;
                const math::Matrix4f& model = entity->world_transform();
                const Material* mat = mr->material.get();
                const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
                math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
                float dist_sq = (pos - cam_pos).length_sq();
                SkinnedDrawItem item{mr->gpu_mesh_handle(), mat, model, mr->palette(), dist_sq};
                if (transparent) {
                    skinned_transparent_items.push_back(std::move(item));
                } else {
                    skinned_opaque_items.push_back(std::move(item));
                }
            });
    }

    // 2c. 不透明物体：blend 关、深度写开
    ctx.set_blend(false);
    ctx.set_depth_write(true);
    for (const auto& item : opaque_items) {
        render_mesh(item.mesh, item.material, item.model, ctx);
    }
    for (const auto& item : skinned_opaque_items) {
        render_skinned_mesh(item.mesh, item.material, item.model, item.palette, ctx);
    }

    // 2d. 透明物体：按到相机距离从远到近排序，blend 开、深度写关
    if (!transparent_items.empty() || !skinned_transparent_items.empty()) {
        std::sort(transparent_items.begin(), transparent_items.end(),
                  [](const DrawItem& a, const DrawItem& b) { return a.dist_sq > b.dist_sq; });
        std::sort(skinned_transparent_items.begin(), skinned_transparent_items.end(),
                  [](const SkinnedDrawItem& a, const SkinnedDrawItem& b) { return a.dist_sq > b.dist_sq; });
        ctx.set_blend(true);
        ctx.set_depth_write(false);
        for (const auto& item : transparent_items) {
            render_mesh(item.mesh, item.material, item.model, ctx);
        }
        for (const auto& item : skinned_transparent_items) {
            render_skinned_mesh(item.mesh, item.material, item.model, item.palette, ctx);
        }
        ctx.set_blend(false);
        ctx.set_depth_write(true);
    }

    if (hdr_enabled_) {
        end_hdr_forward_pass(ctx);
        // 3. Tone mapping pass to backbuffer
        render_tonemap(ctx);
    } else {
        end_forward_pass(ctx);
    }
}

void RenderPipeline::render_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                 RenderContext& ctx) {
    if (!mesh.is_valid() || !pbr_shader_.is_valid() || !camera_) return;

    // 双面材质关闭背面剔除
    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face(!cull_disabled_ && !two_sided);

    ctx.set_shader(pbr_shader_);
    ctx.set_uniform_mat4(pbr_shader_, "uModel", model);
    ctx.set_uniform_mat4(pbr_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(pbr_shader_, "uProjection", camera_->get_projection_matrix());
    ctx.set_uniform_mat4(pbr_shader_, "uLightSpaceMatrix", light_space_matrix_);
    ctx.set_uniform_vec3(pbr_shader_, "uCameraPos", camera_->position());

    // 单独 render mesh 时也确保全局光照参数正确
    ctx.set_uniform_vec3(pbr_shader_, "uAmbient", ambient_);
    ctx.set_uniform_int(pbr_shader_, "uHDREnabled", hdr_enabled_ ? 1 : 0);
    upload_lights(ctx, pbr_shader_);

    if (material) {
        material->bind(&ctx, pbr_shader_);
    }

    const bool use_shadow = shadow_enabled_ && shadow_light_index_ >= 0 && shadow_map_.is_valid();
    if (shadow_map_.is_valid()) {
        ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
        if (shadow_map_ptr) {
            shadow_map_ptr->bind(TextureSlots::kPBRShadow);
        }
        ctx.set_texture(pbr_shader_, shadow_map_, TextureSlots::kPBRShadow, "");
        ctx.set_uniform_int(pbr_shader_, "uShadowMap", TextureSlots::kPBRShadow);
    }
    ctx.set_uniform_int(pbr_shader_, "uUseShadowMap", use_shadow ? 1 : 0);
    ctx.set_uniform_float(pbr_shader_, "uShadowBias", shadow_bias_);

    ctx.draw_mesh(mesh, pbr_shader_);
}

void RenderPipeline::render_skinned_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                         std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                         RenderContext& ctx) {
    if (!mesh.is_valid() || !skinned_pbr_shader_.is_valid() || !camera_) return;

    // 双面材质关闭背面剔除
    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face(!cull_disabled_ && !two_sided);

    ctx.set_shader(skinned_pbr_shader_);
    ctx.set_uniform_mat4(skinned_pbr_shader_, "uModel", model);
    ctx.set_uniform_mat4(skinned_pbr_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(skinned_pbr_shader_, "uProjection", camera_->get_projection_matrix());
    ctx.set_uniform_mat4(skinned_pbr_shader_, "uLightSpaceMatrix", light_space_matrix_);
    ctx.set_uniform_vec3(skinned_pbr_shader_, "uCameraPos", camera_->position());

    // 蒙皮管线与标准 PBR 不共享 program/UBO，必须单独上传全局光照参数
    ctx.set_uniform_vec3(skinned_pbr_shader_, "uAmbient", ambient_);
    ctx.set_uniform_int(skinned_pbr_shader_, "uHDREnabled", hdr_enabled_ ? 1 : 0);
    upload_lights(ctx, skinned_pbr_shader_);

    // palette：shared_ptr 按值捕获进命令队列，渲染线程执行时数据仍有效
    if (palette && !palette->empty()) {
        ctx.set_uniform_mat4_array(skinned_pbr_shader_, "uBonePalette", std::move(palette));
    }

    if (material) {
        material->bind(&ctx, skinned_pbr_shader_);
    }

    const bool use_shadow = shadow_enabled_ && shadow_light_index_ >= 0 && shadow_map_.is_valid();
    if (shadow_map_.is_valid()) {
        ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
        if (shadow_map_ptr) {
            shadow_map_ptr->bind(TextureSlots::kPBRShadow);
        }
        ctx.set_texture(skinned_pbr_shader_, shadow_map_, TextureSlots::kPBRShadow, "");
        ctx.set_uniform_int(skinned_pbr_shader_, "uShadowMap", TextureSlots::kPBRShadow);
    }
    ctx.set_uniform_int(skinned_pbr_shader_, "uUseShadowMap", use_shadow ? 1 : 0);
    ctx.set_uniform_float(skinned_pbr_shader_, "uShadowBias", shadow_bias_);

    ctx.draw_mesh(mesh, skinned_pbr_shader_);
}

void RenderPipeline::begin_shadow_pass(RenderContext& ctx) {
    ctx.set_shader(shadow_shader_);
    ctx.set_framebuffer(shadow_fbo_);
    ctx.set_viewport(0, 0, shadow_map_size_, shadow_map_size_);
    ctx.clear_depth();
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(!cull_disabled_);
}

void RenderPipeline::end_shadow_pass(RenderContext& ctx) {
    ctx.set_framebuffer(RHIFramebufferHandle{});
}

void RenderPipeline::begin_forward_pass(RenderContext& ctx) {
    ctx.set_shader(pbr_shader_);
    ctx.set_framebuffer(RHIFramebufferHandle{});
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(!cull_disabled_);
}

void RenderPipeline::end_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

void RenderPipeline::bind_global_uniforms(RenderContext& ctx) {
    // uniform 命令作用于"当前绑定"的 program（见 GLShader::set_*），
    // skybox pass 可能刚绑定过其它 shader，这里必须先绑回 PBR shader。
    ctx.set_shader(pbr_shader_);
    ctx.set_uniform_vec3(pbr_shader_, "uAmbient", ambient_);
    ctx.set_uniform_int(pbr_shader_, "uHDREnabled", hdr_enabled_ ? 1 : 0);
    upload_lights(ctx, pbr_shader_);
}

void RenderPipeline::upload_lights(RenderContext& ctx, RHIShaderHandle shader) {
    if (!shader.is_valid()) return;
    const int count = static_cast<int>(std::min(lights_.size(), static_cast<size_t>(k_max_lights)));
    ctx.set_uniform_int(shader, "uLightCount", count);
    ctx.set_uniform_int(shader, "uShadowLightIndex", shadow_light_index_);

    char name[48];
    for (int i = 0; i < count; ++i) {
        const Light& light = lights_[i];

        std::snprintf(name, sizeof(name), "uLightType[%d]", i);
        ctx.set_uniform_int(shader, name, static_cast<int>(light.type));

        std::snprintf(name, sizeof(name), "uLightPos[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.position);

        std::snprintf(name, sizeof(name), "uLightDir[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.direction);

        std::snprintf(name, sizeof(name), "uLightColor[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.color);

        std::snprintf(name, sizeof(name), "uLightIntensity[%d]", i);
        ctx.set_uniform_float(shader, name, light.intensity);

        // x=range, y=cos(outer), z=cos(inner)
        const float outer = light.spot_angle * 3.14159265f / 180.0f;
        const float inner = light.spot_angle * (1.0f - light.spot_softness) * 3.14159265f / 180.0f;
        std::snprintf(name, sizeof(name), "uLightParams[%d]", i);
        ctx.set_uniform_vec4(shader, name,
                             math::Vector4f(light.range, std::cos(outer), std::cos(inner), 0.0f));
    }
}

bool RenderPipeline::create_hdr_target(RenderContext* ctx) {
    hdr_color_ = ctx->create_texture();
    ITexture* hdr_color_ptr = ctx->texture(hdr_color_);
    if (!hdr_color_.is_valid() || !hdr_color_ptr || !hdr_color_ptr->create(TextureFormat::RGBA16F, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }
    hdr_color_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    hdr_color_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    hdr_depth_ = ctx->create_texture();
    ITexture* hdr_depth_ptr = ctx->texture(hdr_depth_);
    if (!hdr_depth_.is_valid() || !hdr_depth_ptr || !hdr_depth_ptr->create(TextureFormat::Depth24, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }

    hdr_fbo_ = ctx->create_framebuffer();
    IFramebuffer* hdr_fbo_ptr = ctx->framebuffer(hdr_fbo_);
    if (!hdr_fbo_.is_valid() || !hdr_fbo_ptr || !hdr_fbo_ptr->create(viewport_width_, viewport_height_)) {
        return false;
    }
    hdr_fbo_ptr->attach_color_texture(hdr_color_ptr);
    hdr_fbo_ptr->attach_depth_texture(hdr_depth_ptr);
    if (!hdr_fbo_ptr->is_complete()) {
        GLOG_ERROR("RenderPipeline: HDR framebuffer incomplete");
        return false;
    }
    return true;
}

bool RenderPipeline::create_viewport_target(RenderContext* ctx) {
    // tonemap 输出为 LDR，RGBA8 足够
    viewport_color_ = ctx->create_texture();
    ITexture* color_ptr = ctx->texture(viewport_color_);
    if (!viewport_color_.is_valid() || !color_ptr ||
        !color_ptr->create(TextureFormat::RGBA8, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }
    color_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    color_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    viewport_fbo_ = ctx->create_framebuffer();
    IFramebuffer* fbo_ptr = ctx->framebuffer(viewport_fbo_);
    if (!viewport_fbo_.is_valid() || !fbo_ptr ||
        !fbo_ptr->create(viewport_width_, viewport_height_)) {
        return false;
    }
    fbo_ptr->attach_color_texture(color_ptr);
    if (!fbo_ptr->is_complete()) {
        GLOG_ERROR("RenderPipeline: viewport framebuffer incomplete");
        return false;
    }
    return true;
}

ITexture* RenderPipeline::viewport_color_texture() const {
    if (!ctx_ || !viewport_color_.is_valid()) return nullptr;
    return ctx_->texture(viewport_color_);
}

bool RenderPipeline::resize_render_targets(int width, int height) {
    if (!ctx_ || width <= 0 || height <= 0) return false;
    if (width == viewport_width_ && height == viewport_height_) return true;

    viewport_width_ = width;
    viewport_height_ = height;

    // 先创建新目标再销毁旧目标：销毁命令经 pending 队列延迟执行，
    // 若先销毁再创建，句柄槽位可能被立即复用导致悬垂引用。
    if (hdr_enabled_) {
        RHITextureHandle old_color = hdr_color_;
        RHITextureHandle old_depth = hdr_depth_;
        RHIFramebufferHandle old_fbo = hdr_fbo_;
        hdr_color_ = RHITextureHandle{};
        hdr_depth_ = RHITextureHandle{};
        hdr_fbo_ = RHIFramebufferHandle{};

        if (!create_hdr_target(ctx_)) {
            GLOG_ERROR("RenderPipeline: resize HDR target failed ({}x{})", width, height);
            // 回滚：销毁可能创建了一半的新目标，恢复旧目标
            if (hdr_color_.is_valid()) ctx_->destroy_texture(hdr_color_);
            if (hdr_depth_.is_valid()) ctx_->destroy_texture(hdr_depth_);
            if (hdr_fbo_.is_valid()) ctx_->destroy_framebuffer(hdr_fbo_);
            hdr_color_ = old_color;
            hdr_depth_ = old_depth;
            hdr_fbo_ = old_fbo;
            return false;
        }
        if (old_color.is_valid()) ctx_->destroy_texture(old_color);
        if (old_depth.is_valid()) ctx_->destroy_texture(old_depth);
        if (old_fbo.is_valid()) ctx_->destroy_framebuffer(old_fbo);
    }

    if (viewport_output_enabled_) {
        RHITextureHandle old_color = viewport_color_;
        RHIFramebufferHandle old_fbo = viewport_fbo_;
        viewport_color_ = RHITextureHandle{};
        viewport_fbo_ = RHIFramebufferHandle{};

        if (!create_viewport_target(ctx_)) {
            GLOG_ERROR("RenderPipeline: resize viewport target failed ({}x{})", width, height);
            if (viewport_color_.is_valid()) ctx_->destroy_texture(viewport_color_);
            if (viewport_fbo_.is_valid()) ctx_->destroy_framebuffer(viewport_fbo_);
            viewport_color_ = old_color;
            viewport_fbo_ = old_fbo;
            return false;
        }
        if (old_color.is_valid()) ctx_->destroy_texture(old_color);
        if (old_fbo.is_valid()) ctx_->destroy_framebuffer(old_fbo);
    }
    return true;
}

bool RenderPipeline::create_fullscreen_mesh(RenderContext* ctx) {
    fullscreen_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(fullscreen_mesh_);
    if (!fullscreen_mesh_.is_valid() || !mesh_ptr) return false;

    // 一个覆盖全屏的三角形，包含 position(uv) 和 texcoord
    struct Vertex {
        float x, y;
        float u, v;
    };
    Vertex verts[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f},
        { 3.0f, -1.0f, 2.0f, 0.0f},
        {-1.0f,  3.0f, 0.0f, 2.0f}
    };

    mesh_ptr->upload_vertices(verts, sizeof(verts), 3);
    VertexLayout layout;
    layout.stride = sizeof(Vertex);
    layout.attributes = {
        {0, VertexType::Float2, false, 0},
        {1, VertexType::Float2, false, 2 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::begin_hdr_forward_pass(RenderContext& ctx) {
    ctx.set_shader(pbr_shader_);
    ctx.set_framebuffer(hdr_fbo_);
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.clear(0.15f, 0.15f, 0.18f, 1.0f);
    ctx.clear_depth();
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(!cull_disabled_);
}

void RenderPipeline::end_hdr_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

void RenderPipeline::render_tonemap(RenderContext& ctx) {
    if (!tonemap_shader_.is_valid() || !hdr_color_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    // 编辑器视口输出开启时，tonemap 写入独立 FBO 供 Viewport 面板采样，
    // 默认 framebuffer 只用于 ImGui；否则按原路径直接输出到屏幕。
    const bool to_viewport = viewport_output_enabled_ && viewport_fbo_.is_valid();
    ctx.set_framebuffer(to_viewport ? viewport_fbo_ : RHIFramebufferHandle{});
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    ctx.set_shader(tonemap_shader_);

    ITexture* hdr_color_ptr = ctx_->texture(hdr_color_);
    IShader* tonemap_ptr = ctx_->shader(tonemap_shader_);
    IMesh* fullscreen_ptr = ctx_->mesh(fullscreen_mesh_);
    if (!hdr_color_ptr || !tonemap_ptr || !fullscreen_ptr) return;

    hdr_color_ptr->bind(TextureSlots::kTonemapHDR);
    ctx.set_texture(tonemap_shader_, hdr_color_, TextureSlots::kTonemapHDR, "");
    ctx.set_uniform_int(tonemap_shader_, "uHDRTexture", TextureSlots::kTonemapHDR);
    tonemap_ptr->set_post_process_params(exposure_, tone_map_mode_);

    ctx.draw_mesh(fullscreen_mesh_, tonemap_shader_);
}

// ---------------------------------------------------------------------------
// Scene View 网格线
// ---------------------------------------------------------------------------
bool RenderPipeline::create_grid_mesh(RenderContext* ctx) {
    // 大平面覆盖 XZ 平面，使用 MeshVertex 布局（position 有效，其余填 0）。
    struct VertexGPU {
        float x, y, z;
        float nx, ny, nz;
        float tx, ty, tz;
        float u, v;
        float r, g, b;
    };

    const float half = 500.0f;
    VertexGPU verts[] = {
        {-half, 0.0f, -half, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1},
        { half, 0.0f, -half, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1},
        { half, 0.0f,  half, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1},
        {-half, 0.0f,  half, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1},
    };
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};

    grid_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(grid_mesh_);
    if (!grid_mesh_.is_valid() || !mesh_ptr) return false;
    mesh_ptr->upload_vertices(verts, sizeof(verts), 4);
    mesh_ptr->upload_indices(indices, sizeof(indices), 6);

    VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::render_grid(RenderContext& ctx) {
    if (!grid_enabled_ || !grid_shader_.is_valid() || !grid_mesh_.is_valid() || !camera_) return;

    // 网格透明混合，深度测试开启但深度写入关闭，避免遮挡场景物体。
    ctx.set_depth_test(true);
    ctx.set_depth_write(false);
    ctx.set_cull_face(false);
    ctx.set_blend(true);

    ctx.set_shader(grid_shader_);
    ctx.set_uniform_mat4(grid_shader_, "uModel", math::Matrix4f::identity());
    ctx.set_uniform_mat4(grid_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(grid_shader_, "uProjection", camera_->get_projection_matrix());
    ctx.set_uniform_vec3(grid_shader_, "uGridColor", math::Vector3f(0.5f, 0.5f, 0.5f));
    ctx.set_uniform_float(grid_shader_, "uGridSize", k_grid_size);
    ctx.set_uniform_float(grid_shader_, "uMajorLineEvery", k_grid_major_every);
    ctx.set_uniform_float(grid_shader_, "uFadeStart", k_grid_fade_start);
    ctx.set_uniform_float(grid_shader_, "uFadeEnd", k_grid_fade_end);

    ctx.draw_mesh(grid_mesh_, grid_shader_);

    // 恢复默认状态（不透明物体需要这些状态）
    ctx.set_depth_write(true);
    ctx.set_cull_face(!cull_disabled_);
    ctx.set_blend(false);
}

} // namespace gryce_engine::render
