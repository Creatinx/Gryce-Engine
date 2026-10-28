#include "render/streamline/sl_interface.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

StreamlineInterface& StreamlineInterface::instance() {
    static StreamlineInterface inst;
    return inst;
}

bool StreamlineInterface::init_d3d12(void* /*device*/, void* /*swap_chain*/,
                                    int /*output_width*/, int /*output_height*/) {
#if defined(GRYCE_HAS_STREAMLINE)
    // 真实接入在此：slInit() + slSetFeature 启 DLSS/Reflex + 把 device/swap_chain
    // 交给 SL 的 D3D12 扩展。SDK 二进制就位后填。
    // 注意：RHI 句柄是不透明整数，D3D12 后端的真实 ID3D12Device* 必须经后端内部
    // 拿到（见 d3d12_backend.cpp 的 present 路径），这里只收 void* 透传。
    GLOG_WARN("Streamline: real DLSS/Reflex integration not yet wired (SDK stub)");
    return false;
#else
    // 无 SDK：静默 no-op，调用方后续特性调用自动跳过。
    return false;
#endif
}

void StreamlineInterface::shutdown() {
#if defined(GRYCE_HAS_STREAMLINE)
    // slShutdown()
#endif
}

bool StreamlineInterface::dlss_sr_ready() const {
#if defined(GRYCE_HAS_STREAMLINE)
    return available_ && false; // 真实 ready 判定待 SDK 接入
#else
    return false;
#endif
}

RHITextureHandle StreamlineInterface::dlss_sr_render(RenderContext* /*ctx*/,
                                                    RHITextureHandle color_tex,
                                                    RHITextureHandle /*depth_tex*/,
                                                    RHITextureHandle /*motion_vectors_tex*/,
                                                    RHITextureHandle /*exposure_tex*/,
                                                    const PostProcessParams& /*params*/,
                                                    int /*render_w*/, int /*render_h*/) {
#if defined(GRYCE_HAS_STREAMLINE)
    if (!dlss_sr_ready()) return color_tex;
    // 真实接入：slEvaluateFeature(SL_FEATURE_DLSS) + 把 RHI 资源映射给 SL 的
    // D3D12 标签接口。资源句柄为不透明整数，须由 D3D12 后端内部解析为
    // ID3D12Resource* 才能交给 NGX（见执行计划 P1.2 的接线说明）。
    return color_tex;
#else
    return color_tex;
#endif
}

bool StreamlineInterface::reflex_ready() const {
#if defined(GRYCE_HAS_STREAMLINE)
    return available_ && false;
#else
    return false;
#endif
}

void StreamlineInterface::reflex_mark_gpu_busy_start() {
#if defined(GRYCE_HAS_STREAMLINE)
    // slReflexHelper / SetLatencyMarker 等
#endif
}

void StreamlineInterface::reflex_mark_gpu_busy_end() {
#if defined(GRYCE_HAS_STREAMLINE)
#endif
}

void StreamlineInterface::reflex_set_latency_sleep(bool /*enabled*/) {
#if defined(GRYCE_HAS_STREAMLINE)
#endif
}

} // namespace gryce_engine::render
