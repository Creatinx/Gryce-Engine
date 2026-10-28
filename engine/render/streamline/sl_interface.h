#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// StreamlineInterface — NVIDIA Streamline（DLSS / Reflex）集成包装层
//
// 这是 P1 的"骨架"：定义 DLSS SR / Reflex 的接入面，但真实 SDK 调用被
// GRYCE_HAS_STREAMLINE 守卫。未启用（或无 SDK / 非 D3D12）时，所有方法都是
// 安全的 no-op，dlss_sr_ready() 恒返回 false —— 后处理链里的调用点会据此
// 静默跳过，画面与现状零差异。
//
// 单例：Streamline 本身是单例式 API（slInit 全局一次），故本包装也走单例。
// 资源句柄（RHI 纹理）是不透明整数，D3D12 后端内部的真实 ID3D12Resource*
// 必须经过后端自己解析后才能交给 NGX（见执行计划 P1.2 的接线说明）。
// ---------------------------------------------------------------------------
class StreamlineInterface {
public:
    static StreamlineInterface& instance();

    // 在 D3D12 设备 / 交换链就绪后由后端调用一次，初始化 Streamline 与已启用的特性。
    // 返回是否成功（失败则后续特性调用全部 no-op）。device / swap_chain 以 void*
    // 透传，避免头文件泄漏 SDK 类型；真实实现里再转回 ID3D12Device* / IDXGISwapChain*。
    bool init_d3d12(void* device, void* swap_chain, int output_width, int output_height);

    // 帧末 / 设备销毁时调用，释放 Streamline 资源。
    void shutdown();

    // ---- DLSS 超分辨率（Super Resolution）----
    // 是否已就绪（SDK 接入且特性已启用）。无 SDK 时恒 false。
    bool dlss_sr_ready() const;
    // 把低分辨率 color/depth/motion 上采样到输出分辨率。返回输出纹理句柄：
    // 真正执行时返回上采样结果（调用方据此替换 fb_.color_tex），未执行时
    // 原样返回 color_tex（让后续链走原图）。
    RHITextureHandle dlss_sr_render(RenderContext* ctx,
                                    RHITextureHandle color_tex,
                                    RHITextureHandle depth_tex,
                                    RHITextureHandle motion_vectors_tex,
                                    RHITextureHandle exposure_tex,
                                    const PostProcessParams& params,
                                    int render_w, int render_h);

    // ---- NVIDIA Reflex（低延迟）----
    bool reflex_ready() const;
    void reflex_mark_gpu_busy_start();
    void reflex_mark_gpu_busy_end();
    void reflex_set_latency_sleep(bool enabled);

    // 给 D3D12 后端的特性就绪查询（避免头文件泄漏 SDK 类型）。
    bool available() const { return available_; }

private:
    StreamlineInterface() = default;

    bool available_ = false;
};

} // namespace gryce_engine::render
