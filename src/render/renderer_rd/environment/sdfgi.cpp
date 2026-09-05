#include "render/renderer_rd/environment/sdfgi.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool SDFGI_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void SDFGI_RD::destroy() {
    if (!ctx_) return;
    destroy_cascades();
    initialized_ = false;
}

bool SDFGI_RD::create_cascades(const math::Vector3f& center, float max_distance) {
    destroy_cascades();

    // 创建 SDF 体素纹理（3D 纹理）
    for (int i = 0; i < k_cascade_count; ++i) {
        auto& cascade = cascades_[i];
        cascade.center = center;
        cascade.size = max_distance / (1 << (k_cascade_count - 1 - i));

        // SDF 体素
        cascade.sdf_tex = ctx_->create_texture();
        ITexture* sdf_tex = ctx_->texture(cascade.sdf_tex);
        if (!cascade.sdf_tex.is_valid() || !sdf_tex ||
            !sdf_tex->create(TextureFormat::R8, k_cascade_size, k_cascade_size, nullptr)) {
            return false;
        }

        // GI 体素
        cascade.gi_tex = ctx_->create_texture();
        ITexture* gi_tex = ctx_->texture(cascade.gi_tex);
        if (!cascade.gi_tex.is_valid() || !gi_tex ||
            !gi_tex->create(TextureFormat::RGBA16F, k_cascade_size, k_cascade_size, nullptr)) {
            return false;
        }
    }

    return true;
}

void SDFGI_RD::destroy_cascades() {
    if (!ctx_) return;
    for (auto& cascade : cascades_) {
        if (cascade.sdf_tex.is_valid()) { ctx_->destroy_texture(cascade.sdf_tex); cascade.sdf_tex = {}; }
        if (cascade.gi_tex.is_valid()) { ctx_->destroy_texture(cascade.gi_tex); cascade.gi_tex = {}; }
        if (cascade.irradiance_tex.is_valid()) { ctx_->destroy_texture(cascade.irradiance_tex); cascade.irradiance_tex = {}; }
        if (cascade.fbo.is_valid()) { ctx_->destroy_framebuffer(cascade.fbo); cascade.fbo = {}; }
    }
}

void SDFGI_RD::update(const math::Vector3f& camera_pos,
                       const std::vector<math::Vector3f>& light_directions,
                       const std::vector<math::Vector3f>& light_colors) {
    if (!initialized_) return;

    // TODO: 实现 SDFGI 更新循环
    // 1. 体素化场景几何体到 SDF
    // 2. 从光源注入光照到 GI 体素
    // 3. 各向异性扩散填充 GI 体素
}

} // namespace gryce_engine::render