#include "render/renderer_rd/effects/water.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool Water_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    if (!ctx) return false;
    ctx_ = ctx;

    create_water_mesh();

    // 加载水面 shader
    water_shader_ = ctx->create_shader();
    if (water_shader_.is_valid()) {
        IShader* s = ctx->shader(water_shader_);
        if (s) {
            if (!s->load_program("water", shader_dir, nullptr, true, false)) {
                GLOG_WARN("Water_RD: failed to load water shader from '{}'", shader_dir);
                ctx->destroy_shader(water_shader_);
                water_shader_ = {};
            }
        }
    }

    initialized_ = true;
    GLOG_INFO("Water_RD initialized (shader='{}')", shader_dir);
    return true;
}

void Water_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (water_mesh_.is_valid()) {
        ctx_->destroy_mesh(water_mesh_);
        water_mesh_ = {};
    }
    if (water_shader_.is_valid()) {
        ctx_->destroy_shader(water_shader_);
        water_shader_ = {};
    }
    if (fullscreen_mesh_.is_valid()) {
        ctx_->destroy_mesh(fullscreen_mesh_);
        fullscreen_mesh_ = {};
    }
    initialized_ = false;
    ctx_ = nullptr;
}

void Water_RD::set_water_params(float amplitude, float frequency, float speed,
                                float steepness, float level,
                                const math::Vector3f& color) {
    wave_amplitude_ = amplitude;
    wave_frequency_ = frequency;
    wave_speed_ = speed;
    wave_steepness_ = steepness;
    water_height_ = level;
    water_color_ = color;
}

void Water_RD::create_water_mesh() {
    water_mesh_ = ctx_->create_mesh();
    if (!water_mesh_.is_valid()) {
        GLOG_ERROR("Water_RD: failed to create water mesh handle");
        return;
    }

    IMesh* mesh = ctx_->mesh(water_mesh_);
    if (!mesh) {
        GLOG_ERROR("Water_RD: failed to get water mesh");
        return;
    }

    const int segments = 64;
    const int vertex_count = (segments + 1) * (segments + 1);
    const int index_count = segments * segments * 6;

    // 顶点: position (float3) + normal (float3) + texcoord (float2)
    struct WaterVertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };

    std::vector<WaterVertex> vertices;
    vertices.reserve(vertex_count);

    const float half_size = water_size_ * 0.5f;
    for (int j = 0; j <= segments; ++j) {
        for (int i = 0; i <= segments; ++i) {
            float u = static_cast<float>(i) / segments;
            float v = static_cast<float>(j) / segments;
            float x = -half_size + u * water_size_;
            float z = -half_size + v * water_size_;
            // 法线向上（实际法线在 shader 中计算）
            vertices.push_back({x, 0.0f, z, 0.0f, 1.0f, 0.0f, u, v});
        }
    }

    // 索引
    std::vector<uint32_t> indices;
    indices.reserve(index_count);
    for (int j = 0; j < segments; ++j) {
        for (int i = 0; i < segments; ++i) {
            uint32_t row0 = static_cast<uint32_t>(j * (segments + 1) + i);
            uint32_t row1 = static_cast<uint32_t>((j + 1) * (segments + 1) + i);

            indices.push_back(row0);
            indices.push_back(row0 + 1);
            indices.push_back(row1);

            indices.push_back(row1);
            indices.push_back(row0 + 1);
            indices.push_back(row1 + 1);
        }
    }

    VertexLayout layout;
    layout.stride = sizeof(WaterVertex);
    layout.attributes = {
        {0, VertexType::Float3, false, offsetof(WaterVertex, x)},
        {1, VertexType::Float3, false, offsetof(WaterVertex, nx)},
        {2, VertexType::Float2, false, offsetof(WaterVertex, u)}
    };
    mesh->set_layout(layout);
    mesh->upload_vertices(vertices.data(),
                          static_cast<uint32_t>(vertices.size() * sizeof(WaterVertex)),
                          static_cast<uint32_t>(vertices.size()));
    mesh->upload_indices(indices.data(),
                         static_cast<uint32_t>(indices.size() * sizeof(uint32_t)),
                         static_cast<uint32_t>(indices.size()));

    // 创建全屏四边形 mesh（用于反射/折射 pass）
    fullscreen_mesh_ = ctx_->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* fs_mesh = ctx_->mesh(fullscreen_mesh_);
        if (fs_mesh) {
            struct FullscreenVertex {
                float x, y;
                float u, v;
            };
            FullscreenVertex verts[] = {
                {-1.0f, -1.0f, 0.0f, 0.0f},
                { 3.0f, -1.0f, 2.0f, 0.0f},
                {-1.0f,  3.0f, 0.0f, 2.0f}
            };
            VertexLayout fs_layout;
            fs_layout.stride = sizeof(FullscreenVertex);
            fs_layout.attributes = {
                {0, VertexType::Float2, false, 0},
                {1, VertexType::Float2, false, 2 * sizeof(float)}
            };
            fs_mesh->set_layout(fs_layout);
            fs_mesh->upload_vertices(verts, sizeof(verts), 3);
        }
    }
}

bool Water_RD::create_targets(int width, int height) {
    destroy_targets();
    if (!ctx_) return false;

    // 反射/折射使用半分辨率
    const int half_w = std::max(16, width / 2);
    const int half_h = std::max(16, height / 2);
    reflection_w_ = half_w;
    reflection_h_ = half_h;

    // ---- 反射纹理 ----
    reflection_tex_ = ctx_->create_texture();
    ITexture* ref_tex = ctx_->texture(reflection_tex_);
    if (!reflection_tex_.is_valid() || !ref_tex ||
        !ref_tex->create(TextureFormat::RGBA16F, half_w, half_h, nullptr)) {
        GLOG_ERROR("Water_RD: failed to create reflection texture ({}x{})", half_w, half_h);
        return false;
    }
    ref_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    ref_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    reflection_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* ref_fbo = ctx_->framebuffer(reflection_fbo_);
    if (!reflection_fbo_.is_valid() || !ref_fbo || !ref_fbo->create(half_w, half_h)) {
        GLOG_ERROR("Water_RD: failed to create reflection FBO");
        return false;
    }
    ref_fbo->attach_color_texture(ref_tex);
    if (!ref_fbo->is_complete()) {
        GLOG_ERROR("Water_RD: reflection FBO is not complete");
        return false;
    }

    // ---- 折射纹理 ----
    refraction_tex_ = ctx_->create_texture();
    ITexture* refr_tex = ctx_->texture(refraction_tex_);
    if (!refraction_tex_.is_valid() || !refr_tex ||
        !refr_tex->create(TextureFormat::RGBA16F, half_w, half_h, nullptr)) {
        GLOG_ERROR("Water_RD: failed to create refraction texture ({}x{})", half_w, half_h);
        return false;
    }
    refr_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    refr_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    refraction_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* refr_fbo = ctx_->framebuffer(refraction_fbo_);
    if (!refraction_fbo_.is_valid() || !refr_fbo || !refr_fbo->create(half_w, half_h)) {
        GLOG_ERROR("Water_RD: failed to create refraction FBO");
        return false;
    }
    refr_fbo->attach_color_texture(refr_tex);
    if (!refr_fbo->is_complete()) {
        GLOG_ERROR("Water_RD: refraction FBO is not complete");
        return false;
    }

    // ---- 水输出纹理（全分辨率） ----
    water_w_ = width;
    water_h_ = height;
    water_tex_ = ctx_->create_texture();
    ITexture* w_tex = ctx_->texture(water_tex_);
    if (!water_tex_.is_valid() || !w_tex ||
        !w_tex->create(TextureFormat::RGBA16F, width, height, nullptr)) {
        GLOG_ERROR("Water_RD: failed to create water output texture ({}x{})", width, height);
        return false;
    }
    w_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    w_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    water_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* w_fbo = ctx_->framebuffer(water_fbo_);
    if (!water_fbo_.is_valid() || !w_fbo || !w_fbo->create(width, height)) {
        GLOG_ERROR("Water_RD: failed to create water output FBO");
        return false;
    }
    w_fbo->attach_color_texture(w_tex);
    if (!w_fbo->is_complete()) {
        GLOG_ERROR("Water_RD: water output FBO is not complete");
        return false;
    }

    GLOG_INFO("Water_RD: targets created (reflection {}x{}, refraction {}x{}, water {}x{})",
              half_w, half_h, half_w, half_h, width, height);
    return true;
}

void Water_RD::destroy_targets() {
    if (!ctx_) return;
    if (water_tex_.is_valid()) {
        ctx_->destroy_texture(water_tex_);
        water_tex_ = {};
    }
    if (water_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(water_fbo_);
        water_fbo_ = {};
    }
    if (reflection_tex_.is_valid()) {
        ctx_->destroy_texture(reflection_tex_);
        reflection_tex_ = {};
    }
    if (reflection_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(reflection_fbo_);
        reflection_fbo_ = {};
    }
    if (refraction_tex_.is_valid()) {
        ctx_->destroy_texture(refraction_tex_);
        refraction_tex_ = {};
    }
    if (refraction_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(refraction_fbo_);
        refraction_fbo_ = {};
    }
    reflection_w_ = 512;
    reflection_h_ = 512;
}

void Water_RD::render_reflection(RenderContext* ctx,
                                 const math::Vector3f& camera_pos,
                                 float water_height) {
    if (!initialized_ || !reflection_fbo_.is_valid()) return;

    // 反射渲染：将场景渲染到反射纹理
    // 实际反射渲染由 RenderPipeline 在外部完成（使用反射相机矩阵）
    ctx->set_framebuffer(reflection_fbo_);
    ctx->set_viewport(0, 0, reflection_w_, reflection_h_);
    ctx->clear(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->clear_depth();
}

void Water_RD::render_water(RenderContext* ctx,
                             RHITextureHandle reflection_tex,
                             RHITextureHandle refraction_tex,
                             RHITextureHandle depth_tex,
                             const math::Matrix4f& view_proj,
                             const math::Vector3f& camera_pos,
                             float time,
                             float water_height) {
    if (!initialized_ || !water_mesh_.is_valid() || !water_shader_.is_valid()) return;

    // 将水面网格渲染到当前 FBO（HDR 颜色缓冲区），使用 alpha blend
    ctx->set_shader(water_shader_);

    // 模型矩阵：平移到水面高度
    math::Matrix4f model = math::Matrix4f::identity();
    model(3, 1) = water_height;
    ctx->set_uniform_mat4(water_shader_, "uModel", model);
    ctx->set_uniform_mat4(water_shader_, "uViewProj", view_proj);

    ctx->set_uniform_vec3(water_shader_, "uCameraPos", camera_pos);
    ctx->set_uniform_vec3(water_shader_, "uWaterColor", water_color_);
    ctx->set_uniform_float(water_shader_, "uWaterHeight", water_height);
    ctx->set_uniform_float(water_shader_, "uFoamAmount", water_foam_amount_);

    // 波浪参数
    ctx->set_uniform_float(water_shader_, "uTime", time);
    ctx->set_uniform_float(water_shader_, "uWaveAmplitude", wave_amplitude_);
    ctx->set_uniform_float(water_shader_, "uWaveFrequency", wave_frequency_);
    ctx->set_uniform_float(water_shader_, "uWaveSpeed", wave_speed_);
    ctx->set_uniform_float(water_shader_, "uWaveSteepness", wave_steepness_);

    // 绑定纹理
    static constexpr int kReflectionSlot = 0;
    static constexpr int kRefractionSlot = 1;
    static constexpr int kDepthSlot = 2;

    if (reflection_tex.is_valid()) {
        ctx->set_texture(water_shader_, reflection_tex, kReflectionSlot, "uReflectionTex");
    }
    if (refraction_tex.is_valid()) {
        ctx->set_texture(water_shader_, refraction_tex, kRefractionSlot, "uRefractionTex");
    }
    if (depth_tex.is_valid()) {
        ctx->set_texture_raw_depth(water_shader_, depth_tex, kDepthSlot, "uDepthTex");
    }

    // 设置混合模式：半透明水面
    ctx->set_blend(true);
    ctx->set_depth_write(false);
    ctx->set_depth_test(true);
    ctx->set_cull_face(CullMode::None);

    // 绘制水面网格
    ctx->draw_indexed(water_mesh_, water_shader_);

    // 恢复状态
    ctx->set_blend(false);
    ctx->set_depth_write(true);
    ctx->set_cull_face(CullMode::Back);
}

} // namespace gryce_engine::render