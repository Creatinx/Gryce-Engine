#include "render_pipeline.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include "render/render_context.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"
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

    pbr_shader_ = load_shader("pbr", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!pbr_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load pbr shader");
        return false;
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

    if (hdr_enabled_) {
        if (!create_fullscreen_mesh(ctx)) {
            GLOG_WARN("RenderPipeline: fullscreen mesh failed, falling back to LDR");
            hdr_enabled_ = false;
        }
    }

    initialized_ = true;
    GLOG_INFO("RenderPipeline initialized ({})", hdr_enabled_ ? "PBR + shadow + HDR" : "PBR + shadow");
    return true;
}

void RenderPipeline::shutdown() {
    if (!ctx_) return;

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
    if (owns_shaders_ && tonemap_shader_.is_valid()) {
        ctx_->destroy_shader(tonemap_shader_);
        tonemap_shader_ = RHIShaderHandle{};
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
    }
    pbr_shader_ = RHIShaderHandle{};
    shadow_shader_ = RHIShaderHandle{};
    ctx_ = nullptr;
    initialized_ = false;
}

RHIShaderHandle RenderPipeline::load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process) {
    RHIShaderHandle shader = ctx_->create_shader();
    IShader* shader_ptr = ctx_->shader(shader);
    IFramebuffer* target_ptr = ctx_->framebuffer(target);
    if (!shader.is_valid() || !shader_ptr || !shader_ptr->load_program(name, shader_dir_, target_ptr, color_output, post_process)) {
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
    update_light_space_matrix();
}

void RenderPipeline::set_viewport(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
}

ITexture* RenderPipeline::shadow_map() const {
    return ctx_ ? ctx_->texture(shadow_map_) : nullptr;
}

void RenderPipeline::update_light_space_matrix() {
    if (lights_.empty()) {
        light_space_matrix_ = math::Matrix4f::identity();
        return;
    }

    // 简单定向光 shadow：正交投影覆盖场景中心
    math::Vector3f light_dir = lights_[0].direction.normalized();
    math::Vector3f center = math::Vector3f::zero();
    math::Vector3f eye = center - light_dir * 10.0f;
    math::Matrix4f light_view = math::Matrix4f::look_at(eye, center, math::Vector3f::up());
    math::Matrix4f light_proj = math::Matrix4f::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 20.0f);
    light_space_matrix_ = light_proj * light_view;
}

void RenderPipeline::render_scene(scene::Scene& scene, RenderContext& ctx) {
    if (!initialized_ || !camera_) return;

    // 1. Shadow pass
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

    // 2. Forward PBR pass (HDR target or backbuffer)
    if (hdr_enabled_) {
        begin_hdr_forward_pass(ctx);
    } else {
        begin_forward_pass(ctx);
    }
    bind_global_uniforms(ctx);
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
            render_mesh(mr->gpu_mesh_handle(), mr->material.get(), entity->world_transform(), ctx);
        });
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

    ctx.set_uniform_mat4(pbr_shader_, "uModel", model);
    ctx.set_uniform_mat4(pbr_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(pbr_shader_, "uProjection", camera_->get_projection_matrix());
    ctx.set_uniform_mat4(pbr_shader_, "uLightSpaceMatrix", light_space_matrix_);
    ctx.set_uniform_vec3(pbr_shader_, "uCameraPos", camera_->position());

    if (material) {
        material->bind(&ctx, pbr_shader_);
    }

    if (shadow_map_.is_valid()) {
        ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
        if (shadow_map_ptr) {
            shadow_map_ptr->bind(5);
        }
        ctx.set_texture(pbr_shader_, shadow_map_, 5, "");
        ctx.set_uniform_int(pbr_shader_, "uShadowMap", 5);
        ctx.set_uniform_int(pbr_shader_, "uUseShadowMap", 1);
    } else {
        ctx.set_uniform_int(pbr_shader_, "uUseShadowMap", 0);
    }
    ctx.set_uniform_float(pbr_shader_, "uShadowBias", shadow_bias_);

    ctx.draw_mesh(mesh, pbr_shader_);
}

void RenderPipeline::begin_shadow_pass(RenderContext& ctx) {
    ctx.set_shader(shadow_shader_);
    ctx.set_framebuffer(shadow_fbo_);
    ctx.set_viewport(0, 0, shadow_map_size_, shadow_map_size_);
    ctx.clear_depth();
    ctx.set_depth_test(true);
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
    ctx.set_cull_face(!cull_disabled_);
}

void RenderPipeline::end_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

void RenderPipeline::bind_global_uniforms(RenderContext& ctx) {
    if (lights_.empty()) return;

    const Light& light = lights_[0];
    ctx.set_uniform_vec3(pbr_shader_, "uLightDir", light.direction);
    ctx.set_uniform_vec3(pbr_shader_, "uLightColor", light.color);
    ctx.set_uniform_float(pbr_shader_, "uLightIntensity", light.intensity);
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
    ctx.set_cull_face(!cull_disabled_);
}

void RenderPipeline::end_hdr_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

void RenderPipeline::render_tonemap(RenderContext& ctx) {
    if (!tonemap_shader_.is_valid() || !hdr_color_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    ctx.set_framebuffer(RHIFramebufferHandle{});
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    ctx.set_shader(tonemap_shader_);

    ITexture* hdr_color_ptr = ctx_->texture(hdr_color_);
    IShader* tonemap_ptr = ctx_->shader(tonemap_shader_);
    IMesh* fullscreen_ptr = ctx_->mesh(fullscreen_mesh_);
    if (!hdr_color_ptr || !tonemap_ptr || !fullscreen_ptr) return;

    hdr_color_ptr->bind(0);
    ctx.set_texture(tonemap_shader_, hdr_color_, 0, "");
    ctx.set_uniform_int(tonemap_shader_, "uHDRTexture", 0);
    tonemap_ptr->set_post_process_params(exposure_, tone_map_mode_);

    ctx.draw_mesh(fullscreen_mesh_, tonemap_shader_);
}

} // namespace gryce_engine::render
