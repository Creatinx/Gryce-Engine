#include "d3d12_renderer2d.h"

#include "render/d3d12/d3d12_backend.h"
#include "render/d3d12/d3d12_mesh.h"
#include "render/render.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

D3D12Renderer2D::D3D12Renderer2D(D3D12Backend* backend) {
    d3d12_backend_ = backend;
}

void D3D12Renderer2D::begin_overlay_pass(int screen_width, int screen_height) {
    // 3D 编辑器视口使用后端内部 present 目标承接屏幕内容。进入 UI overlay 前
    // 先把该目标翻转到真实 back buffer，随后 UI 才能稳定地直接画到交换链；
    // 否则离屏输出 + UI 的组合会让 overlay 继续写进 present 目标并在截图/呈现
    // 路径中丢失。
    if (d3d12_backend_ && ctx()) {
        D3D12Backend* backend = d3d12_backend_;
        ctx()->push_command([backend](IRenderBackend*) {
            backend->prepare_gryce_ui_overlay();
        });
    }
    Renderer2D::begin_overlay_pass(screen_width, screen_height);
}

void D3D12Renderer2D::submit_mesh(IMesh* mesh, IShader* shader,
                                  RHIMeshHandle mesh_handle,
                                  RHIShaderHandle shader_handle) {
    (void)mesh;
    if (d3d12_backend_) d3d12_backend_->draw_mesh(mesh_handle, shader_handle);
    if (shader) shader->unbind();
}

void D3D12Renderer2D::configure_batch_mesh(IMesh* mesh) {
    if (auto* d3d_mesh = dynamic_cast<d3d12::D3D12Mesh*>(mesh)) {
        d3d_mesh->set_streaming_vertices(true);
    }
}

// ---------------------------------------------------------------------------
// 深度纹理的采样配置
//
// GL 需要显式关闭 TEXTURE_COMPARE_MODE（否则 sampler2D 取到的是比较结果而非
// 深度值）。D3D12 的比较行为由 sampler 描述符的 ComparisonFunc 决定，而
// ComparisonFunc 只在 filter 是「比较滤波器」时生效；2D 阴影用的采样器走普通
// 的 MIN_MAG_MIP_LINEAR，天然不做比较。所以这里无需额外动作。
// ---------------------------------------------------------------------------
void D3D12Renderer2D::configure_depth_texture_for_sampling(ITexture* shadow_map) {
    (void)shadow_map;
}

// D3D12 的 framebuffer = 一组 RTV/DSV；只 attach 了 DSV 的目标在 OMSetRenderTargets
// 时 RTV 数量即为 0，不存在 GL 的「默认颜色附件被写」问题。
void D3D12Renderer2D::disable_color_writes_for_depth_only_target() {}

// ---------------------------------------------------------------------------
// 1x1 默认纹理：直接走 RenderContext 的通用纹理工厂，落在 D3D12Texture 上。
// 基类钩子签名是 uint32_t，这里用「资源池 index + 1」作为 id（0 = 无效）。
// ---------------------------------------------------------------------------
uint32_t D3D12Renderer2D::create_default_1x1_texture(const unsigned char rgba[4]) {
    if (!ctx()) return 0;

    RHITextureHandle tex = ctx()->create_texture();
    if (!tex.is_valid()) {
        GLOG_ERROR("D3D12Renderer2D: failed to create 1x1 default texture");
        return 0;
    }
    ITexture* tex_ptr = ctx()->texture(tex);
    if (!tex_ptr || !tex_ptr->upload_data(rgba, 1, 1, 4)) {
        GLOG_ERROR("D3D12Renderer2D: failed to upload 1x1 default texture");
        ctx()->destroy_texture(tex);
        return 0;
    }
    tex_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    // 两个槽位（albedo / normal）按调用顺序占位。
    const int slot = created_defaults_ ? 1 : 0;
    default_textures_[slot] = tex;
    created_defaults_ = true;
    return tex.index + 1u;
}

void D3D12Renderer2D::destroy_default_texture(uint32_t id) {
    if (id == 0 || !ctx()) return;
    const uint32_t index = id - 1u;
    for (RHITextureHandle& h : default_textures_) {
        if (h.is_valid() && h.index == index) {
            ctx()->destroy_texture(h);
            h = RHITextureHandle{};
            return;
        }
    }
}

void D3D12Renderer2D::bind_default_texture_slot(int slot, uint32_t id) {
    if (id == 0 || !ctx()) return;
    const uint32_t index = id - 1u;
    for (const RHITextureHandle& h : default_textures_) {
        if (h.is_valid() && h.index == index) {
            if (ITexture* tex = ctx()->texture(h)) {
                tex->bind(static_cast<uint32_t>(slot));
            }
            return;
        }
    }
}

} // namespace gryce_engine::render
