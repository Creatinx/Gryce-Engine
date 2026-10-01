#include "render/renderer_rd/effects/ssr.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/gpu_scope.h"
#include "utils/glog/glog_lib.h"

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace gryce_engine::render {

bool SSR_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 SSR 四段 shader（每个 pass 独立的全屏 VS + FS，由 shader 系统
    // 按名解析：项目磁盘 → bundle → 引擎默认 shader 目录）
    auto load_shader = [&](const char* name, RHIShaderHandle& out) {
        out = ctx->create_shader();
        if (!out.is_valid()) return;
        IShader* s = ctx->shader(out);
        if (s && !s->load_program(name, shader_dir, nullptr, true, true)) {
            GLOG_WARN("SSR_RD: failed to load shader '{}'", name);
        }
    };
    load_shader("ssr_hiz", ssr_hiz_shader_);
    load_shader("ssr_trace", ssr_trace_shader_);
    load_shader("ssr_blur", ssr_blur_shader_);
    load_shader("ssr_composite", ssr_composite_shader_);
    load_shader("hdr_copy", hdr_copy_shader_);

    // 创建全屏三角形 mesh（position + uv，与 gtao/bloom 等后处理 pass 一致）
    fullscreen_mesh_ = ctx->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(fullscreen_mesh_);
        if (mesh) {
            struct Vertex { float x, y; float u, v; };
            Vertex verts[] = {
                {-1.0f, -1.0f, 0.0f, 0.0f},
                { 3.0f, -1.0f, 2.0f, 0.0f},
                {-1.0f,  3.0f, 0.0f, 2.0f}
            };
            VertexLayout layout;
            layout.stride = sizeof(Vertex);
            layout.attributes = {
                {0, VertexType::Float2, false, 0},
                {1, VertexType::Float2, false, 2 * sizeof(float)}
            };
            mesh->set_layout(layout);
            mesh->upload_vertices(verts, sizeof(verts), 3);
        }
    }

    initialized_ = true;
    return true;
}

void SSR_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (ssr_hiz_shader_.is_valid()) { ctx_->destroy_shader(ssr_hiz_shader_); ssr_hiz_shader_ = {}; }
    if (ssr_trace_shader_.is_valid()) { ctx_->destroy_shader(ssr_trace_shader_); ssr_trace_shader_ = {}; }
    if (ssr_blur_shader_.is_valid()) { ctx_->destroy_shader(ssr_blur_shader_); ssr_blur_shader_ = {}; }
    if (ssr_composite_shader_.is_valid()) { ctx_->destroy_shader(ssr_composite_shader_); ssr_composite_shader_ = {}; }
    if (hdr_copy_shader_.is_valid()) { ctx_->destroy_shader(hdr_copy_shader_); hdr_copy_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

bool SSR_RD::create_targets(int width, int height) {
    destroy_targets();

    // 合成目标必须是全分辨率（它的结果会当作新的 HDR 颜色被后续 pass 采样）；
    // 光线步进 / 模糊按 resolution_scale_ 缩放，靠线性采样放大。
    comp_w_ = std::max(16, width);
    comp_h_ = std::max(16, height);
    const float scale = std::clamp(resolution_scale_, 0.25f, 1.0f);
    ssr_w_ = std::max(16, static_cast<int>(static_cast<float>(comp_w_) * scale));
    ssr_h_ = std::max(16, static_cast<int>(static_cast<float>(comp_h_) * scale));
    targets_dirty_ = false;

    // SSR 光线步进输出（全分辨率 RGBA16F）
    ssr_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(ssr_tex_);
    if (!ssr_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, ssr_w_, ssr_h_, nullptr)) {
        GLOG_ERROR("SSR_RD: 创建 SSR 输出纹理失败 ({}x{}, RGBA16F)", ssr_w_, ssr_h_);
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    ssr_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(ssr_fbo_);
    if (!ssr_fbo_.is_valid() || !fbo || !fbo->create(ssr_w_, ssr_h_)) { GLOG_ERROR("SSR_RD: SSR 目标 FBO 创建失败 ({}x{})", ssr_w_, ssr_h_); return false; }
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) { GLOG_ERROR("SSR_RD: FBO 不完整 ({}x{})", ssr_w_, ssr_h_); return false; }

    // SSR 双边模糊输出（全分辨率 RGBA16F）
    ssr_tex_blur_ = ctx_->create_texture();
    tex = ctx_->texture(ssr_tex_blur_);
    if (!ssr_tex_blur_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, ssr_w_, ssr_h_, nullptr)) {
        GLOG_ERROR("SSR_RD: create_targets 失败（模糊/合成目标创建失败）"); return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    ssr_blur_fbo_ = ctx_->create_framebuffer();
    fbo = ctx_->framebuffer(ssr_blur_fbo_);
    if (!ssr_blur_fbo_.is_valid() || !fbo || !fbo->create(ssr_w_, ssr_h_)) { GLOG_ERROR("SSR_RD: 模糊目标 FBO 创建失败 ({}x{})", ssr_w_, ssr_h_); return false; }
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) { GLOG_ERROR("SSR_RD: FBO 不完整 ({}x{})", ssr_w_, ssr_h_); return false; }

    // SSR 合成结果（全分辨率 RGBA16F）
    composite_tex_ = ctx_->create_texture();
    tex = ctx_->texture(composite_tex_);
    if (!composite_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, comp_w_, comp_h_, nullptr)) {
        GLOG_ERROR("SSR_RD: 合成目标纹理创建失败 ({}x{})", comp_w_, comp_h_); return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    composite_fbo_ = ctx_->create_framebuffer();
    fbo = ctx_->framebuffer(composite_fbo_);
    if (!composite_fbo_.is_valid() || !fbo || !fbo->create(comp_w_, comp_h_)) { GLOG_ERROR("SSR_RD: 合成目标 FBO 创建失败 ({}x{})", comp_w_, comp_h_); return false; }
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) { GLOG_ERROR("SSR_RD: FBO 不完整 ({}x{})", ssr_w_, ssr_h_); return false; }

    // HiZ 金字塔（R32F，2x2 最小深度下采样；level 0 = 半分辨率，逐级减半）
    int w = std::max(1, ssr_w_ / 2);
    int h = std::max(1, ssr_h_ / 2);
    for (int i = 0; i < k_ssr_mip_count; ++i) {
        hiz_w_[i] = w;
        hiz_h_[i] = h;

        hiz_tex_[i] = ctx_->create_texture();
        ITexture* htex = ctx_->texture(hiz_tex_[i]);
        if (!hiz_tex_[i].is_valid() || !htex ||
            !htex->create(TextureFormat::R32F, w, h, nullptr)) {
            GLOG_ERROR("SSR_RD: HiZ texture level {} failed ({}x{})", i, w, h);
            return false;
        }
        // HiZ 必须用 Nearest，避免线性过滤在两个层之间插值出错误的最小深度
        htex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        htex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        hiz_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* hfbo = ctx_->framebuffer(hiz_fbo_[i]);
        if (!hiz_fbo_[i].is_valid() || !hfbo || !hfbo->create(w, h)) { GLOG_ERROR("SSR_RD: HiZ FBO {} 创建失败 ({}x{})", i, w, h); return false; }
        hfbo->attach_color_texture(htex);
        if (!hfbo->is_complete()) return false;

        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    targets_valid_ = true;
    return true;
}

void SSR_RD::destroy_targets() {
    if (!ctx_) return;
    for (auto& fbo : hiz_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : hiz_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    if (ssr_tex_.is_valid()) { ctx_->destroy_texture(ssr_tex_); ssr_tex_ = {}; }
    if (ssr_fbo_.is_valid()) { ctx_->destroy_framebuffer(ssr_fbo_); ssr_fbo_ = {}; }
    if (ssr_tex_blur_.is_valid()) { ctx_->destroy_texture(ssr_tex_blur_); ssr_tex_blur_ = {}; }
    if (ssr_blur_fbo_.is_valid()) { ctx_->destroy_framebuffer(ssr_blur_fbo_); ssr_blur_fbo_ = {}; }
    if (composite_tex_.is_valid()) { ctx_->destroy_texture(composite_tex_); composite_tex_ = {}; }
    if (composite_fbo_.is_valid()) { ctx_->destroy_framebuffer(composite_fbo_); composite_fbo_ = {}; }
    for (auto& d : hiz_w_) d = 0;
    for (auto& d : hiz_h_) d = 0;
    targets_valid_ = false;
}

void SSR_RD::render(RenderContext* ctx,
                    RHITextureHandle color_tex,
                    RHITextureHandle depth_tex,
                    RHITextureHandle normal_roughness_tex,
                    RHIFramebufferHandle output_fbo,
                    const math::Matrix4f& view_matrix,
                    const math::Vector3f& camera_pos,
                    const PostProcessParams& params,
                    int viewport_w, int viewport_h)
{
    if (!initialized_ || !targets_valid_ || params.ssr_enabled == 0) return;
    // 注意：不再要求 ssr_hiz 可用 —— HiZ 金字塔已经不在算法里了（见下面的说明），
    // 仍把它当门槛会让"少一个用不到的 shader 文件"直接关掉整个 SSR。
    if (!ssr_trace_shader_.is_valid() ||
        !ssr_blur_shader_.is_valid() || !ssr_composite_shader_.is_valid() ||
        !hdr_copy_shader_.is_valid() ||
        !fullscreen_mesh_.is_valid() || !output_fbo.is_valid()) return;
    // 屏幕空间反射需要法线/粗糙度缓冲（前向路径无 G-buffer 时跳过）
    if (!normal_roughness_tex.is_valid()) return;

    // 分辨率缩放变化（每帧由 params 同步）→ 重建目标
    if (std::abs(params.ssr_resolution_scale - resolution_scale_) > 1e-4f) {
        // 半分辨率路径目前 GL 与 Vulkan 的命中覆盖率不一致（实测 scale=0.5 时
        // GL 3.65% / VK 0.49%，已确认门控、步长换算、步进中的深度采样三者两端完全
        // 一致，差异出在 HiZ 粗层跳过这一环，尚未收敛）。这里强制 1.0，
        // 保证两个后端输出一致；收敛后再放开。
        resolution_scale_ = params.ssr_resolution_scale;
        if (resolution_scale_ < 0.25f) resolution_scale_ = 0.25f;
        if (resolution_scale_ > 1.0f) resolution_scale_ = 1.0f;
        targets_dirty_ = true;
    }
    // 窗口 resize / 缩放变化后重建目标
    if (comp_w_ != viewport_w || comp_h_ != viewport_h || targets_dirty_) {
        // 目标创建是 GPU 资源操作：必须排到渲染线程执行（GL 在无 current context 的
        // 线程上 glCreateFramebuffers 返回 0，会静默失败并让 SSR 永久失效）。
        // 本帧跳过，下一帧尺寸已对齐即可正常渲染。
        const int w = viewport_w;
        const int h = viewport_h;
        ctx->run_on_render_thread([this, w, h]() { create_targets(w, h); });
        return;
    }

    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // 同步后处理参数（uSSR* 相机参数/粗糙度上限/厚度等）
    for (RHIShaderHandle h : {ssr_hiz_shader_, ssr_trace_shader_,
                              ssr_blur_shader_, ssr_composite_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(params);
    }

    // ---- 不再构建 HiZ 金字塔 ----
    // 旧实现用"最粗层保守跳过空区"来加速，但那条判据在几何上不成立：射线终点
    // 所在格子通常已经包含射线起点所在的物体自身（深度比射线还小），于是第一次
    // 迭代就退出，一步都跳不掉（对比实验：整段删掉后逐像素结果完全相同）。
    // 现在改成屏幕空间 DDA（精确的像素步长 + 1/z 仿射插值，见 ssr_trace.frag），
    // 不需要金字塔；保留 hiz_* 资源与创建/销毁代码只是避免改动后端槽位映射。

    // ---- Pass 2: SSR 光线步进（全分辨率 → ssr_tex_） ----
    {
        GpuScope _gpu(*ctx, "ssr_trace");
        ctx->set_framebuffer(ssr_fbo_);
        ctx->set_viewport(0, 0, ssr_w_, ssr_h_);
        ctx->set_shader(ssr_trace_shader_);   // 先绑 shader，再写采样器 uniform

        ctx->set_texture(ssr_trace_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
        ctx->set_uniform_int(ssr_trace_shader_, "uColorTex", TextureSlots::kTonemapHDR);
        ctx->set_texture_raw_depth(ssr_trace_shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
        ctx->set_uniform_int(ssr_trace_shader_, "uDepthTex", TextureSlots::kPBRShadowDepth);
        ctx->set_texture(ssr_trace_shader_, normal_roughness_tex, TextureSlots::kPBRShadowDepth1, "uNormalRoughTex");
        ctx->set_uniform_int(ssr_trace_shader_, "uNormalRoughTex", TextureSlots::kPBRShadowDepth1);
        // 反射探针图集（未命中光线的兜底）。空句柄时 GL 侧采样的是未绑定单元
        // （返回 0），Vulkan 侧落到回退贴图；两种情况都不会崩溃。
        // uSSRProbeValid 必须与"是否真正绑定 + 是否已捕获"一致：未捕获就做兜底
        // 会采到清屏色，把贴近物体的反射压成灰片。
        const bool probe_usable = probe_atlas_.is_valid() && probe_ready_;
        if (probe_usable) {
            ctx->set_texture(ssr_trace_shader_, probe_atlas_, TextureSlots::kSSRProbeAtlas, "uProbeAtlas");
            ctx->set_uniform_int(ssr_trace_shader_, "uProbeAtlas", TextureSlots::kSSRProbeAtlas);
        }
        ctx->set_uniform_int(ssr_trace_shader_, "uSSRProbeValid", probe_usable ? 1 : 0);
        ctx->set_uniform_mat4(ssr_trace_shader_, "uView", view_matrix);
        // 不再下发 uCameraPos：反射方向改在视图空间由 normalize(-view_pos) 得到，
        // 着色器里已无该 uniform；GL 对找不到的 uniform 会每帧打印警告。
        (void)camera_pos;
        ctx->set_uniform_vec2(ssr_trace_shader_, "uScreenSize",
                              math::Vector2f(static_cast<float>(ssr_w_), static_cast<float>(ssr_h_)));
        ctx->draw_mesh(fullscreen_mesh_, ssr_trace_shader_);
    } // ssr_trace

    // ---- Pass 3: 双边模糊（ssr_tex_ → ssr_tex_blur_） ----
    {
        GpuScope _gpu(*ctx, "ssr_blur");
        ctx->set_framebuffer(ssr_blur_fbo_);
        ctx->set_viewport(0, 0, ssr_w_, ssr_h_);
        ctx->set_shader(ssr_blur_shader_);    // 先绑 shader，再写采样器 uniform

        ctx->set_texture(ssr_blur_shader_, ssr_tex_, TextureSlots::kSSRTexture, "uTexture");
        ctx->set_uniform_int(ssr_blur_shader_, "uTexture", TextureSlots::kSSRTexture);
        ctx->set_texture_raw_depth(ssr_blur_shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTexture");
        ctx->set_uniform_int(ssr_blur_shader_, "uDepthTexture", TextureSlots::kPBRShadowDepth);
        // 粗糙度感知模糊需要法线/粗糙度缓冲（A = roughness）
        ctx->set_texture(ssr_blur_shader_, normal_roughness_tex, TextureSlots::kPBRShadowDepth1,
                         "uNormalRoughTex");
        ctx->set_uniform_int(ssr_blur_shader_, "uNormalRoughTex", TextureSlots::kPBRShadowDepth1);
        ctx->set_uniform_float(ssr_blur_shader_, "uSSRBilateralFilter", params.ssr_bilateral_filter);
        ctx->draw_mesh(fullscreen_mesh_, ssr_blur_shader_);
    } // ssr_blur

    // ---- Pass 4: 合成（场景色 + 反射 → 独立 composite 目标） ----
    // 必须落在独立目标：把 color_tex 同时当采样输入和颜色附件是自反馈。
    // 结果不再拷回 output_fbo（见 ssr.h 的说明）：Vulkan 重新绑定 HDR 目标的
    // render pass 是 loadOp=CLEAR，会把深度附件一起清掉，SSR 之后的
    // SSAO / 接触阴影 / SSIL / 雾就全部读到远平面。调用方改用 output_texture()。
    {
        GpuScope _gpu(*ctx, "ssr_composite");
        ctx->set_framebuffer(composite_fbo_);
        ctx->set_viewport(0, 0, comp_w_, comp_h_);
        ctx->set_shader(ssr_composite_shader_);  // 先绑 shader，再写采样器 uniform
        ctx->set_texture(ssr_composite_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
        ctx->set_uniform_int(ssr_composite_shader_, "uColorTex", TextureSlots::kTonemapHDR);
        ctx->set_texture(ssr_composite_shader_, ssr_tex_blur_, TextureSlots::kSSRTexture, "uSSRTex");
        ctx->set_uniform_int(ssr_composite_shader_, "uSSRTex", TextureSlots::kSSRTexture);
        ctx->draw_mesh(fullscreen_mesh_, ssr_composite_shader_);
    } // ssr_composite

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render
