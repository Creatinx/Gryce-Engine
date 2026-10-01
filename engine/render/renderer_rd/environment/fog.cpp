#include "render/renderer_rd/environment/fog.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/gpu_scope.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VolumetricFog_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 fog shader（命名式加载：由 resolver 按当前 API 解析 GL/vulkan 变体）。
    // fog 的 GL/vulkan 变体都位于 shader_dir 的 forward_clustered 子目录，故拼接子目录。
    const std::string fog_dir = shader_dir + "/forward_clustered";
    fog_shader_ = ctx->create_shader();
    if (fog_shader_.is_valid()) {
        IShader* s = ctx->shader(fog_shader_);
        if (s) {
            if (!s->load_program("fog", fog_dir, nullptr, true, true)) {
                GLOG_WARN("VolumetricFog: failed to load fog shader");
            }
        }
    }

    // 加载 fog apply shader
    fog_apply_shader_ = ctx->create_shader();
    if (fog_apply_shader_.is_valid()) {
        IShader* s = ctx->shader(fog_apply_shader_);
        if (s) {
            if (!s->load_program("fog_apply", fog_dir, nullptr, true, true)) {
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
            // 第二个参数是"字节数"，第三个才是元素个数。这里必须传整个数组的
            // 字节大小，否则 VBO/IBO 只会写入第一个元素的数据，其余顶点读取越界
            // 得到 0，三角形全部退化成零面积 → 全屏合成 draw 不产生任何像素写入。
            mesh->upload_vertices(quad_verts, sizeof(quad_verts), 4);
            mesh->upload_indices(quad_indices, sizeof(quad_indices), 6);
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
    // 与 RenderPipeline::resize_render_targets_impl 同一约定：先建新目标，最后才销毁
    // 旧目标。销毁命令是排进 pending 队列延迟执行的，若先销毁再创建，新句柄会复用
    // 刚释放的槽位，随后那条延迟销毁就会把新创建的对象删掉（表现为雾/合成目标在
    // resize 后悄悄失效、画面变黑）。
    const RHITextureHandle old_fog_tex = fog_tex_;
    const RHIFramebufferHandle old_fog_fbo = fog_fbo_;
    const RHITextureHandle old_depth_tex = depth_down_tex_;
    const RHIFramebufferHandle old_depth_fbo = depth_down_fbo_;
    const RHITextureHandle old_apply_tex = apply_tex_;
    const RHIFramebufferHandle old_apply_fbo = apply_fbo_;
    fog_tex_ = {};
    fog_fbo_ = {};
    depth_down_tex_ = {};
    depth_down_fbo_ = {};
    apply_tex_ = {};
    apply_fbo_ = {};

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
        GLOG_WARN("[fog] create_targets: fog volume texture creation failed");
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

    // 合成输出目标（全分辨率）。必须独立于 hdr_color_：apply pass 要采样场景颜色，
    // 直接把 hdr_color_ 当颜色附件会形成反馈环。
    apply_tex_ = ctx_->create_texture();
    ITexture* atex = ctx_->texture(apply_tex_);
    if (!apply_tex_.is_valid() || !atex ||
        !atex->create(TextureFormat::RGBA16F, viewport_w, viewport_h, nullptr)) {
        GLOG_WARN("[fog] create_targets: apply tex failed");
        return false;
    }
    atex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    atex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    apply_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* afbo = ctx_->framebuffer(apply_fbo_);
    if (!apply_fbo_.is_valid() || !afbo ||
        !afbo->create(viewport_w, viewport_h)) return false;
    afbo->attach_color_texture(atex);
    if (!afbo->is_complete()) return false;

    // 新目标全部就绪后才销毁旧目标（见函数开头说明）
    if (old_fog_tex.is_valid()) ctx_->destroy_texture(old_fog_tex);
    if (old_fog_fbo.is_valid()) ctx_->destroy_framebuffer(old_fog_fbo);
    if (old_depth_tex.is_valid()) ctx_->destroy_texture(old_depth_tex);
    if (old_depth_fbo.is_valid()) ctx_->destroy_framebuffer(old_depth_fbo);
    if (old_apply_tex.is_valid()) ctx_->destroy_texture(old_apply_tex);
    if (old_apply_fbo.is_valid()) ctx_->destroy_framebuffer(old_apply_fbo);

    return true;
}

void VolumetricFog_RD::destroy_targets() {
    if (!ctx_) return;
    if (fog_tex_.is_valid()) { ctx_->destroy_texture(fog_tex_); fog_tex_ = {}; }
    if (fog_fbo_.is_valid()) { ctx_->destroy_framebuffer(fog_fbo_); fog_fbo_ = {}; }
    if (apply_tex_.is_valid()) { ctx_->destroy_texture(apply_tex_); apply_tex_ = {}; }
    if (apply_fbo_.is_valid()) { ctx_->destroy_framebuffer(apply_fbo_); apply_fbo_ = {}; }
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
    if (!initialized_ || !fog_shader_.is_valid() || !fog_tex_.is_valid()) {
        return;
    }
    GpuScope _gpu(*ctx, "fog");

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
    // 必须先 set_shader：GL 的 glUniform1i/glUniformMatrix4fv 作用于"当前绑定
    // 的 program"，只写 location 不绑定的话，uniform 会被写进上一个 pass 的
    // program（用 fog_apply 的 location 去改 pbr 的 uniform），雾参数全部丢失。
    ctx->set_shader(shader);
    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_view_proj);
    ctx->set_uniform_mat4(shader, "uViewMatrix", view_matrix);
    ctx->set_uniform_vec3(shader, "uCameraPos", camera_pos);
    ctx->set_uniform_vec3(shader, "uFogColor", fog_color);
    ctx->set_uniform_float(shader, "uFogDensity", fog_density);
    ctx->set_uniform_float(shader, "uFogHeight", fog_height);
    ctx->set_uniform_vec2(shader, "uFogRange", math::Vector2f(fog_near, fog_far));
    ctx->set_uniform_int(shader, "uFogSliceCount", k_fog_resolution_z);

    // 绑定深度纹理（按"原始深度"绑定：GL 侧需要非比较 sampler2D）。
    // 必须用 TextureSlots 常量而不是裸 0/1/2：Vulkan 的后处理路径按
    // post_process_binding() 把"引擎槽位"折算成固定的描述符 binding，
    // 未登记的裸槽位一律落到 binding 0，多个 sampler 会互相覆盖。
    ctx->set_texture_raw_depth(shader, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
    ctx->set_uniform_int(shader, "uDepthTex", TextureSlots::kPBRShadowDepth);

    // 逐切片渲染
    int slice_w = k_fog_resolution_x;
    int slice_h = k_fog_resolution_y;
    for (int slice = 0; slice < k_fog_resolution_z; ++slice) {
        // 设置视口到当前切片位置
        ctx->set_viewport(slice * slice_w, 0, slice_w, slice_h);
        ctx->set_uniform_int(shader, "uFogSliceIndex", slice);
        ctx->set_uniform_vec2(shader, "uScreenSize",
                              math::Vector2f(static_cast<float>(slice_w), static_cast<float>(slice_h)));

        // 绘制全屏四边形
        if (fullscreen_mesh_.is_valid()) {
            ctx->draw_mesh(fullscreen_mesh_, shader);
        }
    }

    ctx->set_depth_test(true);
}

void VolumetricFog_RD::render_apply(RenderContext* ctx,
                                    RHIFramebufferHandle target_fbo,
                                    int target_width, int target_height,
                                    RHITextureHandle scene_color_tex,
                                    RHITextureHandle depth_tex,
                                    const math::Matrix4f& inv_view_proj,
                                    const math::Vector3f& camera_pos,
                                    float fog_near, float fog_far) {
    if (!initialized_ || !fog_apply_shader_.is_valid() || !fog_tex_.is_valid() ||
        !target_fbo.is_valid()) {
        return;
    }
    GpuScope _gpu(*ctx, "fog_apply");

    RHIShaderHandle shader = fog_apply_shader_;

    // 必须显式绑定目标与视口：render() 结束时当前 FBO 是 fog_fbo_、视口落在最后一个
    // 切片区域，沿用会把合成结果画进雾体积纹理的角落，场景颜色完全看不到雾
    // （合成"零效果"的根因之一）。
    ctx->set_framebuffer(target_fbo);
    ctx->set_viewport(0, 0, target_width, target_height);
    // 先绑 program：下面所有 set_uniform_* 都作用于"当前 program"（GL 语义）。
    ctx->set_shader(shader);
    // 全屏合成必须关闭面剔除：全屏四边形的绕序/当前剔除状态由上一个 pass 决定，
    // 若被剔除则整屏保持未写入的黑色（合成"整屏变黑"的根因）。
    ctx->set_cull_face(CullMode::None);
    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_blend(false);

    // 绑定输入纹理。必须传 shader 句柄：Vulkan 需要它来更新 descriptor set，
    // GL 侧也借同一命令把 sampler uniform 指到正确单元。
    // 槽位一律走 TextureSlots 常量：Vulkan 的 post_process_binding() 只把登记过的
    // 槽位映射到各自的描述符 binding，裸 0/1/2 会全部映射到 binding 0，导致
    // uSceneColor 实际采到深度图（整屏被"红雾"淹没的表现）。
    ctx->set_texture(shader, scene_color_tex, TextureSlots::kTonemapHDR, "uSceneColor");
    ctx->set_uniform_int(shader, "uSceneColor", TextureSlots::kTonemapHDR);
    ctx->set_texture(shader, fog_tex_, TextureSlots::kSSRTexture, "uFogTex");
    ctx->set_uniform_int(shader, "uFogTex", TextureSlots::kSSRTexture);
    ctx->set_texture_raw_depth(shader, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
    ctx->set_uniform_int(shader, "uDepthTex", TextureSlots::kPBRShadowDepth);

    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_view_proj);
    ctx->set_uniform_vec3(shader, "uCameraPos", camera_pos);
    // 切片索引由视图距离换算，必须按同一个 [0, far] 区间缩放（fog.frag 的切片
    // 边界即 (i/N)*far），否则索引恒为 0、只在近平面附近取样。
    ctx->set_uniform_vec2(shader, "uFogRange", math::Vector2f(fog_near, fog_far));
    ctx->set_uniform_int(shader, "uFogSliceCount", k_fog_resolution_z);

    if (fullscreen_mesh_.is_valid()) {
        ctx->draw_mesh(fullscreen_mesh_, shader);
    }

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render
