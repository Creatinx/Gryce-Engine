#pragma once

#include "render/renderer2d_impl.h"

namespace gryce_engine::render {

class Renderer2D;  // 定义在 render/renderer2d_impl.h

namespace d3d12 { class D3D12Backend; }
using d3d12::D3D12Backend;

// ---------------------------------------------------------------------------
// D3D12Renderer2D — D3D12 后端的 2D 渲染器
//
// 复用了 Renderer2D（renderer2d_impl）全部后端无关的逻辑（批处理、正交矩阵、
// 2D 光照参数、硬阴影、bloom ping-pong、字体图集），只覆盖少数几个需要直接
// 触碰设备的钩子。2D 渲染器的规模由此从「照抄一份 1600 行」降到「几十行」。
//
// 覆盖的钩子：
//   * use_backend_framebuffers()  —— D3D12 的 offscreen RT 由引擎 pass 负责
//     barrier；2D 的 renderer 不能再自己切换 RT（与 Vulkan 一致，见
//     Renderer2D::render_lit_sprites_forward 的 target_is_scene_fbo 分支）。
//   * create_default_1x1_texture / destroy_ / bind_default_texture_slot
//     —— 走 ctx_->create_texture()（D3D12Texture 资源池），等价于 GL 的
//        glGenTextures / glBindTextureUnit。
//   * configure_depth_texture_for_sampling / disable_color_writes_for_depth_only_target
//     —— GL 专有的两个动作，D3D12 下天然成立，做空实现。
// ---------------------------------------------------------------------------
class D3D12Renderer2D final : public Renderer2D {
public:
    explicit D3D12Renderer2D(D3D12Backend* backend);
    ~D3D12Renderer2D() override = default;

protected:
    bool use_backend_framebuffers() const override { return true; }

    void configure_depth_texture_for_sampling(ITexture* shadow_map) override;
    void disable_color_writes_for_depth_only_target() override;

    uint32_t create_default_1x1_texture(const unsigned char rgba[4]) override;
    void destroy_default_texture(uint32_t id) override;
    void bind_default_texture_slot(int slot, uint32_t id) override;

private:
    // 1x1 默认纹理的引擎纹理句柄（albedo / normal 各一个）。
    // 基类的钩子签名是 uint32_t，这里用 index+1 作为 id，0 保留为"无效"。
    RHITextureHandle default_textures_[2];
    bool created_defaults_ = false;
};

} // namespace gryce_engine::render
