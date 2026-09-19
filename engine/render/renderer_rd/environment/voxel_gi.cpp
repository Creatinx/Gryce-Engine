#include "render/renderer_rd/environment/voxel_gi.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VoxelGI_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void VoxelGI_RD::destroy() {
    if (!ctx_) return;
    destroy_voxels();
    initialized_ = false;
}

bool VoxelGI_RD::create(const math::Vector3f& bounds_min, const math::Vector3f& bounds_max,
                         int voxel_size) {
    destroy_voxels();

    bounds_min_ = bounds_min;
    bounds_max_ = bounds_max;
    voxel_size_ = voxel_size;

    // 创建体素 3D 纹理
    // 使用 RGBA16F 存储颜色 + 法线信息
    voxel_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(voxel_tex_);
    if (!voxel_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, voxel_size, voxel_size, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    return true;
}

void VoxelGI_RD::destroy_voxels() {
    if (!ctx_) return;
    if (voxel_tex_.is_valid()) { ctx_->destroy_texture(voxel_tex_); voxel_tex_ = {}; }
    if (irradiance_tex_.is_valid()) { ctx_->destroy_texture(irradiance_tex_); irradiance_tex_ = {}; }
    if (fbo_.is_valid()) { ctx_->destroy_framebuffer(fbo_); fbo_ = {}; }
}

void VoxelGI_RD::inject_light(const math::Vector3f& position, const math::Vector3f& color,
                               float intensity, const math::Vector3f& direction) {
    if (!initialized_) return;
    // TODO: 实现光照注入到体素纹理
    // 使用计算 shader 将光照写入 voxel_tex_
}

} // namespace gryce_engine::render