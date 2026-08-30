#include "render/renderer_rd/effects/motion_blur.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------

bool MotionBlur_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 shader - 使用 fog.vert 作为全屏顶点着色器
    std::string vs = shader_dir + "/fog.vert";

    // 运动向量 shader：从深度重建世界位置，计算屏幕空间运动
    std::string mv_fs = shader_dir + "/motion_vectors.frag";
    motion_vectors_shader_ = ctx->create_shader();
    if (motion_vectors_shader_.is_valid()) {
        IShader* s = ctx->shader(motion_vectors_shader_);
        if (s) {
            if (!s->load_program(vs.c_str(), mv_fs.c_str())) {
                GLOG_WARN("MotionBlur: failed to load motion_vectors shader");
            }
        }
    }

    // 运动模糊 shader：沿运动向量方向做径向模糊
    std::string mb_fs = shader_dir + "/motion_blur.frag";
    motion_blur_shader_ = ctx->create_shader();
    if (motion_blur_shader_.is_valid()) {
        IShader* s = ctx->shader(motion_blur_shader_);
        if (s) {
            if (!s->load_program(vs.c_str(), mb_fs.c_str())) {
                GLOG_WARN("MotionBlur: failed to load motion_blur shader");
            }
        }
    }

    // 创建全屏 mesh
    fullscreen_mesh_ = ctx->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(fullscreen_mesh_);
        if (mesh) {
            float verts[] = { -1.0f, -1.0f, 0.0f,  1.0f, -1.0f, 0.0f,  -1.0f, 1.0f, 0.0f,  1.0f, 1.0f, 0.0f };
            uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
            VertexLayout layout;
            layout.stride = 3 * sizeof(float);
            layout.attributes = { {0, VertexType::Float3, false, 0} };
            mesh->set_layout(layout);
            mesh->upload_vertices(verts, 3 * sizeof(float), 4);
            mesh->upload_indices(indices, sizeof(uint32_t), 6);
        }
    }

    initialized_ = true;
    return true;
}

void MotionBlur_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (motion_vectors_shader_.is_valid()) { ctx_->destroy_shader(motion_vectors_shader_); motion_vectors_shader_ = {}; }
    if (motion_blur_shader_.is_valid()) { ctx_->destroy_shader(motion_blur_shader_); motion_blur_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// targets
// ---------------------------------------------------------------------------

bool MotionBlur_RD::create_targets(int width, int height) {
    destroy_targets();

    blur_w_ = std::max(16, width);
    blur_h_ = std::max(16, height);
    int half_w = std::max(8, width / 2);
    int half_h = std::max(8, height / 2);

    // 全分辨率运动模糊输出
    {
        blur_tex_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(blur_tex_);
        if (!blur_tex_.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, blur_w_, blur_h_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        blur_fbo_ = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(blur_fbo_);
        if (!blur_fbo_.is_valid() || !fbo || !fbo->create(blur_w_, blur_h_)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    // 半分辨率运动向量缓冲
    {
        mv_tex_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(mv_tex_);
        if (!mv_tex_.is_valid() || !tex ||
            !tex->create(TextureFormat::RG16F, half_w, half_h, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        mv_fbo_ = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(mv_fbo_);
        if (!mv_fbo_.is_valid() || !fbo || !fbo->create(half_w, half_h)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    targets_valid_ = true;
    return true;
}

void MotionBlur_RD::destroy_targets() {
    if (!ctx_) return;
    if (blur_tex_.is_valid()) { ctx_->destroy_texture(blur_tex_); blur_tex_ = {}; }
    if (blur_fbo_.is_valid()) { ctx_->destroy_framebuffer(blur_fbo_); blur_fbo_ = {}; }
    if (mv_tex_.is_valid()) { ctx_->destroy_texture(mv_tex_); mv_tex_ = {}; }
    if (mv_fbo_.is_valid()) { ctx_->destroy_framebuffer(mv_fbo_); mv_fbo_ = {}; }
    targets_valid_ = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void MotionBlur_RD::render(RenderContext* ctx,
                           RHITextureHandle color_tex,
                           RHITextureHandle depth_tex,
                           RHITextureHandle motion_vectors_tex,
                           const PostProcessParams& params,
                           int viewport_w, int viewport_h)
{
    if (!initialized_ || !targets_valid_) return;
    if (!motion_blur_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    // 确保目标大小匹配
    if (blur_w_ != viewport_w || blur_h_ != viewport_h) {
        create_targets(viewport_w, viewport_h);
    }

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // 同步后处理参数
    for (RHIShaderHandle h : {motion_vectors_shader_, motion_blur_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(params);
    }

    // Pass 1: 计算运动向量（如果外部未提供）
    bool use_external_mv = motion_vectors_tex.is_valid();
    RHITextureHandle mv_input = motion_vectors_tex;

    if (!use_external_mv && motion_vectors_shader_.is_valid()) {
        int half_w = std::max(8, viewport_w / 2);
        int half_h = std::max(8, viewport_h / 2);

        ctx->set_framebuffer(mv_fbo_);
        ctx->set_viewport(0, 0, half_w, half_h);
        ctx->set_texture_raw_depth(motion_vectors_shader_, depth_tex, TextureSlots::kTAAHistory, "uDepthTex");
        ctx->set_uniform_int(motion_vectors_shader_, "uDepthTex", TextureSlots::kTAAHistory);
        ctx->push_command([h = motion_vectors_shader_, w = static_cast<float>(viewport_w), hh = static_cast<float>(viewport_h)](IRenderBackend* backend) {
            IShader* s = backend->shader(h);
            if (s) s->set_vec2("uScreenSize", math::Vector2f(w, hh));
        });
        ctx->draw_mesh(fullscreen_mesh_, motion_vectors_shader_);
        mv_input = mv_tex_;
    }

    // Pass 2: 运动模糊
    ctx->set_framebuffer(blur_fbo_);
    ctx->set_viewport(0, 0, viewport_w, viewport_h);
    ctx->set_texture(motion_blur_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
    ctx->set_uniform_int(motion_blur_shader_, "uColorTex", TextureSlots::kTonemapHDR);
    ctx->set_texture(motion_blur_shader_, mv_input, TextureSlots::kMotionVectors, "uMotionVecTex");
    ctx->set_uniform_int(motion_blur_shader_, "uMotionVecTex", TextureSlots::kMotionVectors);
    ctx->set_uniform_float(motion_blur_shader_, "uMotionBlurAmount", params.motion_blur_amount);
    ctx->push_command([h = motion_blur_shader_, w = static_cast<float>(viewport_w), hh = static_cast<float>(viewport_h)](IRenderBackend* backend) {
        IShader* s = backend->shader(h);
        if (s) s->set_vec2("uScreenSize", math::Vector2f(w, hh));
    });
    ctx->draw_mesh(fullscreen_mesh_, motion_blur_shader_);
}

} // namespace gryce_engine::render