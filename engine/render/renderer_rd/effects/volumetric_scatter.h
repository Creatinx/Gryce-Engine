#pragma once

#include <string>

#include "render/rhi_handle.h"
#include "render/shader.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// VolumetricScatter_RD — 体积光散射（丁达尔 / 神光 / 光柱）
//
// 与 RenderPipeline::render_volumetric 同一算法（同一套 shader），供
// RenderForwardClustered 的 RD 后处理链使用：沿视线在参与介质中按光线步进，
// 对方向光做单散射累积（Henyey-Greenstein 相位），每个采样点的可见性用屏幕
// 深度近似 —— 这就是"按光线去做"的丁达尔模拟。
//
// 输出 = 输入场景色 + 散射光，落在自己的目标上（不能直接写回 HDR 目标的颜色
// 附件：那是采样与颜色附件同一纹理的反馈环）。调用方应把 output_texture()
// 接成后续 pass 的颜色输入（与 SSR_RD / DOF 同一约定）。
// ---------------------------------------------------------------------------
class VolumetricScatter_RD {
public:
    VolumetricScatter_RD() = default;
    ~VolumetricScatter_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir);
    void destroy();

    bool create_targets(int width, int height);
    void destroy_targets();

    // view_proj 必须是"含后端 z 重映射的最终投影 × 视图"（与深度缓冲同源）。
    // shadow_map / light_space 是平行光级联 0 的阴影贴图与光照矩阵（用于日照
    // 遮挡，光柱由此产生）；传空句柄即退化成均匀薄雾。
    // sun_direction 指向太阳（= -方向光的传播方向）。ndc_z_scale/bias 由调用方
    // 按后端给：GL/WebGL 为 (2,-1)，Vulkan/D3D12 为 (1,0)。
    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle shadow_map,
                const math::Matrix4f& view_proj,
                const math::Matrix4f& light_space,
                const math::Vector3f& camera_pos,
                const math::Vector3f& sun_direction,
                const math::Vector3f& sun_color,
                float sun_intensity,
                float ndc_z_scale, float ndc_z_bias,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    RHITextureHandle output_texture() const { return out_tex_; }
    // 与 SSR_RD 同一条约定：targets 的就绪状态不参与 valid()，否则 init 之后
    // create_targets 之前就被判死，targets 永远建不起来。
    bool valid() const { return initialized_; }

private:
    RenderContext* ctx_ = nullptr;

    RHITextureHandle out_tex_;
    RHIFramebufferHandle out_fbo_;
    int out_w_ = 0;
    int out_h_ = 0;

    RHIShaderHandle shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render
