#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/shader.h"

// NVIDIA Streamline 头文件仅在本文件内包含，且只在 GRYCE_HAS_STREAMLINE 下。
// 无 SDK 时本头文件不依赖任何 SL 符号，保持 SDK 无关。
#if defined(GRYCE_HAS_STREAMLINE)
#  include "sl.h"
#  include "sl_dlss.h"
#  include "sl_reflex.h"
#endif

namespace gryce_engine::render {

class RenderContext;

// D3D12 资源解析回调：把不透明的 RHI 纹理句柄解析成原生 ID3D12Resource* 及状态。
// 由 D3D12 后端在 init_d3d12 时注册（只有后端知道 RHI 句柄 → 原生资源的映射）。
struct D3D12ResolvedResource {
    void*    native = nullptr;   // ID3D12Resource*
    uint32_t state  = 0;         // D3D12_RESOURCE_STATES
    uint32_t width  = 0;
    uint32_t height = 0;
};
using D3D12ResourceResolver = D3D12ResolvedResource (*)(void* rhi_texture_handle);

// 每帧传给 DLSS 的相机/常量数据（unjittered，SL 要求矩阵不含 jitter）。
struct DLSSFrameInputs {
    math::Matrix4f current_view_proj;   // 当前帧 view * proj（不含 jitter）
    math::Matrix4f prev_view_proj;      // 上一帧 view * proj（不含 jitter）
    math::Vector3f camera_pos{};
    float jitter_x_px = 0.0f;           // 像素空间 jitter（与几何 pass 一致）
    float jitter_y_px = 0.0f;
    bool  camera_reset = false;         // 切镜/跳变时打 true，清空历史
};

// ---------------------------------------------------------------------------
// StreamlineInterface — NVIDIA Streamline（DLSS / Reflex）集成包装层
//
// 集成范式：Streamline 是单例式 C API，本包装走单例。核心函数经
// sl.interposer.dll 动态加载（LoadLibrary + GetProcAddress），特性函数（如
// slDLSSSetOptions）经 slGetFeatureFunction 取得。这样绕开了"链接 MSVC 导入库"
// 的路子——本项目 MinGW/GCC 工具链无法稳定链接 MSVC .lib（D3D12 后端同理走
// LoadLibrary），故 Streamline 也采用同一路线。
//
// 二进制 SDK（bin/ 下的 sl.interposer.dll、nvngx_dlss.dll 等）不入库，运行时需
// 放在可执行文件旁；lib/ 下的导入库不链（动态加载不需要）。
//
// 无 SDK（GRYCE_HAS_STREAMLINE 未定义）时，所有方法安全 no-op，dlss_sr_ready()
// 恒 false，后处理链静默跳过，画面零变化。
// ---------------------------------------------------------------------------
class StreamlineInterface {
public:
    static StreamlineInterface& instance();

    // D3D12 后端注册资源解析器（RHI 句柄 → 原生 D3D12 资源）。
    static void set_d3d12_resource_resolver(D3D12ResourceResolver fn);

    // 在 D3D12 设备 / 交换链就绪后由后端调用一次。device 为 void* 透传
    // （内部转回 ID3D12Device*）。返回是否成功（失败则特性调用全部 no-op）。
    bool init_d3d12(void* device, int output_width, int output_height);

    // 设备销毁 / 退出时调用。
    void shutdown();

    // ---- DLSS 超分辨率（Super Resolution）----
    bool dlss_sr_ready() const;
    // 把低分辨率 color/depth/motion 上采样到输出分辨率，返回输出纹理句柄
    // （未执行时返回传入的 color_tex）。exposure 为可选（可传无效句柄）。
    RHITextureHandle dlss_sr_render(RenderContext* ctx,
                                    RHITextureHandle color_tex,
                                    RHITextureHandle depth_tex,
                                    RHITextureHandle motion_vectors_tex,
                                    RHITextureHandle exposure_tex,
                                    const PostProcessParams& params,
                                    const DLSSFrameInputs& f,
                                    int render_w, int render_h);

    // ---- NVIDIA Reflex（低延迟）----
    bool reflex_ready() const;
    void reflex_mark_gpu_busy_start();
    void reflex_mark_gpu_busy_end();
    void reflex_set_latency_sleep(bool enabled);

    bool available() const { return available_; }

private:
    StreamlineInterface() = default;

#if defined(GRYCE_HAS_STREAMLINE)
    bool load_core_api();
    // 把 math::Matrix4f（列主序）写入 sl::float4x4（行主序）。
    static void write_row_major(sl::float4x4& dst, const math::Matrix4f& src);
    bool ensure_output_target(RenderContext* ctx, int out_w, int out_h);
    void destroy_output_target(RenderContext* ctx);
#endif

    bool available_ = false;
    bool initialized_ = false;
    int  output_width_  = 0;
    int  output_height_ = 0;

    RHITextureHandle     output_tex_;
    RHIFramebufferHandle output_fbo_;

#if defined(GRYCE_HAS_STREAMLINE)
    static D3D12ResourceResolver resolver_;

    // 核心 API 的 PFun_ 类型与 DLSS/Reflex 特性函数的 PFun_ 类型都定义在全局
    // 命名空间（sl_core_api.h / sl_dlss.h / sl_reflex.h 里的内联 sl::Xxx 包了
    // 同一组全局函数指针类型）。
    PFun_slInit*               slInit_fn = nullptr;
    PFun_slShutdown*           slShutdown_fn = nullptr;
    PFun_slSetD3DDevice*       slSetD3DDevice_fn = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction_fn = nullptr;
    PFun_slSetConstants*       slSetConstants_fn = nullptr;
    PFun_slGetNewFrameToken*   slGetNewFrameToken_fn = nullptr;
    PFun_slSetTagForFrame*     slSetTagForFrame_fn = nullptr;
    PFun_slEvaluateFeature*    slEvaluateFeature_fn = nullptr;
    PFun_slIsFeatureSupported* slIsFeatureSupported_fn = nullptr;
    PFun_slSetFeatureLoaded*   slSetFeatureLoaded_fn = nullptr;
    PFun_slIsFeatureLoaded*    slIsFeatureLoaded_fn = nullptr;
    PFun_slDLSSSetOptions*     slDLSSSetOptions_fn = nullptr;
#endif
};

} // namespace gryce_engine::render
