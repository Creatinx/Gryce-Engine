#include "render/renderer_rd/effects/ssr.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool SSR_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;

    // Shader 加载（由场景 shader 系统统一管理，这里只做句柄预留）
    // 实际 shader 由 RenderForwardClustered 的 shader_system_ 加载
    // 这里创建全屏四边形 mesh
    // TODO: 使用现有的全屏四边形 mesh 或独立创建

    initialized_ = true;
    return true;
}

void SSR_RD::destroy() {
    if (!ctx_) return;
    destroy_hiz();
    if (ssr_tex_.is_valid()) { ctx_->destroy_texture(ssr_tex_); ssr_tex_ = {}; }
    if (ssr_fbo_.is_valid()) { ctx_->destroy_framebuffer(ssr_fbo_); ssr_fbo_ = {}; }
    initialized_ = false;
}

bool SSR_RD::create_hiz(int width, int height) {
    destroy_hiz();

    // 创建 HiZ Mip 链（从 1/2 分辨率逐级减半）
    hiz_w_ = width;
    hiz_h_ = height;
    int w = std::max(1, width / 2);
    int h = std::max(1, height / 2);

    for (int i = 0; i < k_ssr_mip_count; ++i) {
        hiz_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(hiz_tex_[i]);
        if (!hiz_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::R8, w, h, nullptr)) {
            GLOG_ERROR("SSR_RD: HiZ texture mip {} failed ({}x{})", i, w, h);
            return false;
        }
        tex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    return true;
}

void SSR_RD::destroy_hiz() {
    if (!ctx_) return;
    for (auto& tex : hiz_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    hiz_w_ = 0;
    hiz_h_ = 0;
}

void SSR_RD::render(RenderContext* ctx,
                    RHITextureHandle color_tex,
                    RHITextureHandle depth_tex,
                    RHITextureHandle normal_roughness_tex,
                    const PostProcessParams& params,
                    int viewport_w, int viewport_h)
{
    if (!initialized_ || params.ssr_enabled == 0) return;

    // 1. 构建 HiZ（从深度缓冲构建最小深度 Mip 链）
    // TODO: 使用 HiZ 构建 shader
    // for (int i = 0; i < k_ssr_mip_count; ++i) {
    //     ctx->set_framebuffer(hiz_fbo[i]);
    //     ctx->set_texture(hiz_build_shader_, depth_tex, ...);
    //     ctx->draw_mesh(fullscreen_mesh_, hiz_build_shader_);
    // }

    // 2. SSR 光线步进
    // 使用 HiZ 加速的屏幕空间反射：
    // - 从法线/粗糙度 buffer 计算反射方向
    // - 在 HiZ 中进行层次化光线步进
    // - 输出反射颜色到 ssr_tex_

    // 3. 双边滤波平滑
    // TODO: 使用 SSR 模糊 shader 进行双边滤波
}

} // namespace gryce_engine::render