#include "render/renderer_rd/environment/fog.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VolumetricFog_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 fog shader
    std::string fog_vs = shader_dir + "/fog.vert";
    std::string fog_fs = shader_dir + "/fog.frag";
    fog_shader_ = ctx->create_shader();
    if (fog_shader_.is_valid()) {
        IShader* s = ctx->shader(fog_shader_);
        if (s) {
            if (!s->load_program(fog_vs.c_str(), fog_fs.c_str())) {
                GLOG_WARN("VolumetricFog: failed to load fog shader");
            }
        }
    }

    // 加载 fog apply shader
    std::string fog_apply_fs = shader_dir + "/fog_apply.frag";
    fog_apply_shader_ = ctx->create_shader();
    if (fog_apply_shader_.is_valid()) {
        IShader* s = ctx->shader(fog_apply_shader_);
        if (s) {
            if (!s->load_program(fog_vs.c_str(), fog_apply_fs.c_str())) {
                GLOG_WARN("VolumetricFog: failed to load fog_apply shader");
            }
        }
    }

    // 创建全屏四边形 mesh
    fullscreen_mesh_ = ctx->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(fullscreen_mesh_);
        if (mesh) {
            float quad_verts[] = {
                -1.0f, -1.0f, 0.0f,
                 1.0f, -1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f,
                 1.0f,  1.0f, 0.0f
            };
            uint32_t quad_indices[] = { 0, 1, 2, 2, 1, 3 };
            VertexLayout layout;
            layout.stride = 3 * sizeof(float);
            layout.attributes = { {0, VertexType::Float3, false, 0} };
            mesh->set_layout(layout);
            mesh->upload_vertices(quad_verts, 3 * sizeof(float), 4);
            mesh->upload_indices(quad_indices, sizeof(uint32_t), 6);
        }
    }

    initialized_ = true;
    return true;
}

void VolumetricFog_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (fog_shader_.is_valid()) { ctx_->destroy_shader(fog_shader_); fog_shader_ = {}; }
    if (fog_apply_shader_.is_valid()) { ctx_->destroy_shader(fog_apply_shader_); fog_apply_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

bool VolumetricFog_RD::create_targets(int viewport_w, int viewport_h) {
    destroy_targets();

    // 降采样深度（1/4 分辨率）
    int dw = std::max(16, viewport_w / 4);
    int dh = std::max(16, viewport_h / 4);

    depth_down_tex_ = ctx_->create_texture();
    ITexture* dtex = ctx_->texture(depth_down_tex_);
    if (!depth_down_tex_.is_valid() || !dtex ||
        !dtex->create(TextureFormat::Depth24, dw, dh, nullptr)) {
        return false;
    }

    depth_down_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* dfbo = ctx_->framebuffer(depth_down_fbo_);
    if (!depth_down_fbo_.is_valid() || !dfbo || !dfbo->create(dw, dh)) return false;
    dfbo->attach_depth_texture(dtex);
    if (!dfbo->is_complete()) return false;

    // Fog 3D 体积（使用 2D 纹理模拟 3D volume，切片水平排列）
    int fog_w = k_fog_resolution_x * k_fog_resolution_z;
    int fog_h = k_fog_resolution_y;

    fog_tex_ = ctx_->create_texture();
    ITexture* ftex = ctx_->texture(fog_tex_);
    if (!fog_tex_.is_valid() || !ftex ||
        !ftex->create(TextureFormat::RGBA16F, fog_w, fog_h, nullptr)) {
        return false;
    }
    ftex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    ftex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    fog_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* ffbo = ctx_->framebuffer(fog_fbo_);
    if (!fog_fbo_.is_valid() || !ffbo ||
        !ffbo->create(fog_w, fog_h)) return false;
    ffbo->attach_color_texture(ftex);
    if (!ffbo->is_complete()) return false;

    return true;
}

void VolumetricFog_RD::destroy_targets() {
    if (!ctx_) return;
    if (fog_tex_.is_valid()) { ctx_->destroy_texture(fog_tex_); fog_tex_ = {}; }
    if (fog_fbo_.is_valid()) { ctx_->destroy_framebuffer(fog_fbo_); fog_fbo_ = {}; }
    if (depth_down_tex_.is_valid()) { ctx_->destroy_texture(depth_down_tex_); depth_down_tex_ = {}; }
    if (depth_down_fbo_.is_valid()) { ctx_->destroy_framebuffer(depth_down_fbo_); depth_down_fbo_ = {}; }
}

void VolumetricFog_RD::render(RenderContext* ctx,
                              RHITextureHandle depth_tex,
                              const math::Matrix4f& inv_view_proj,
                              const math::Matrix4f& view_matrix,
                              const math::Vector3f& camera_pos,
                              const math::Vector3f& fog_color,
                              float fog_density, float fog_height,
                              float fog_near, float fog_far) {
    if (!initialized_ || !fog_shader_.is_valid() || !fog_tex_.is_valid()) return;

    // 1. 降采样深度
    ctx->set_framebuffer(depth_down_fbo_);
    ctx->set_viewport(0, 0, depth_down_tex_.is_valid() ? ctx->texture(depth_down_tex_)->width() : 64,
                                 depth_down_tex_.is_valid() ? ctx->texture(depth_down_tex_)->height() : 64);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    // 简单深度拷贝：使用全屏 mesh 采样原始深度
    // 简化：直接使用原始深度，不做降采样

    // 2. 渲染 fog 体积
    // 逐切片渲染 fog 到 3D 纹理
    RHIShaderHandle shader = fog_shader_;

    // 绑定深度纹理和法线纹理
    ctx->set_framebuffer(fog_fbo_);
    int fog_w = k_fog_resolution_x * k_fog_resolution_z;
    int fog_h = k_fog_resolution_y;
    ctx->set_viewport(0, 0, fog_w, fog_h);
    ctx->clear(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_blend(false);

    // 设置 shader uniforms
    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_view_proj);
    ctx->set_uniform_mat4(shader, "uViewMatrix", view_matrix);
    ctx->set_uniform_vec3(shader, "uCameraPos", camera_pos);
    ctx->set_uniform_vec3(shader, "uFogColor", fog_color);
    ctx->set_uniform_float(shader, "uFogDensity", fog_density);
    ctx->set_uniform_float(shader, "uFogHeight", fog_height);
    ctx->push_command([shader, near_val = fog_near, far_val = fog_far](IRenderBackend* backend) {
        IShader* s = backend->shader(shader);
        if (s) s->set_vec2("uFogRange", math::Vector2f(near_val, far_val));
    });
    ctx->set_uniform_int(shader, "uFogSliceCount", k_fog_resolution_z);

    // 绑定深度纹理
    ctx->set_uniform_int(shader, "uDepthTex", 0);
    ctx->set_texture({}, depth_tex, 0, nullptr);

    // 逐切片渲染
    int slice_w = k_fog_resolution_x;
    int slice_h = k_fog_resolution_y;
    for (int slice = 0; slice < k_fog_resolution_z; ++slice) {
        // 设置视口到当前切片位置
        ctx->set_viewport(slice * slice_w, 0, slice_w, slice_h);
        ctx->set_uniform_int(shader, "uFogSliceIndex", slice);
        ctx->push_command([shader, w = static_cast<float>(slice_w), h = static_cast<float>(slice_h)](IRenderBackend* backend) {
            IShader* s = backend->shader(shader);
            if (s) s->set_vec2("uScreenSize", math::Vector2f(w, h));
        });

        // 绘制全屏四边形
        if (fullscreen_mesh_.is_valid()) {
            ctx->draw_mesh(fullscreen_mesh_, shader);
        }
    }

    ctx->set_depth_test(true);
}

void VolumetricFog_RD::render_apply(RenderContext* ctx,
                                    RHITextureHandle scene_color_tex,
                                    RHITextureHandle depth_tex,
                                    const math::Matrix4f& inv_view_proj,
                                    const math::Vector3f& camera_pos) {
    if (!initialized_ || !fog_apply_shader_.is_valid() || !fog_tex_.is_valid()) return;

    RHIShaderHandle shader = fog_apply_shader_;

    // 合成 fog 到场景颜色
    // 注意：这里假设场景颜色已经被绑定为当前渲染目标
    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_blend(false);

    // 绑定输入纹理
    ctx->set_uniform_int(shader, "uSceneColor", 0);
    ctx->set_texture({}, scene_color_tex, 0, nullptr);
    ctx->set_uniform_int(shader, "uFogTex", 1);
    ctx->set_texture({}, fog_tex_, 1, nullptr);
    ctx->set_uniform_int(shader, "uDepthTex", 2);
    ctx->set_texture({}, depth_tex, 2, nullptr);

    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_view_proj);
    ctx->set_uniform_vec3(shader, "uCameraPos", camera_pos);
    ctx->set_uniform_int(shader, "uFogSliceCount", k_fog_resolution_z);

    if (fullscreen_mesh_.is_valid()) {
        ctx->draw_mesh(fullscreen_mesh_, shader);
    }

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render