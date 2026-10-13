#ifndef GRYCE_RENDER_API_H
#define GRYCE_RENDER_API_H

#include "api/types.h"

#ifdef _WIN32
    #ifdef GRYCE_RENDERER_BUILDING
        #define GRYCE_RENDERER_API __declspec(dllexport)
    #else
        #define GRYCE_RENDERER_API __declspec(dllimport)
        #ifdef _MSC_VER
            #ifdef _DEBUG
                #pragma comment(lib, "GryceRendererd.lib")
            #else
                #pragma comment(lib, "GryceRenderer.lib")
            #endif
        #endif
    #endif
#else
    #define GRYCE_RENDERER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t     version;
    GWindowHandle native_window;
    GRenderAPI   api;
    int          viewport_w;
    int          viewport_h;
    bool         sync_mode;
} GRenderInitDesc;

GRYCE_RENDERER_API int  GRender_Init(const GRenderInitDesc* desc);
GRYCE_RENDERER_API void GRender_Shutdown(void);
GRYCE_RENDERER_API bool GRender_IsInitialized(void);

GRYCE_RENDERER_API void GRender_BeginFrame(void);
GRYCE_RENDERER_API void GRender_RenderWorld(void);
GRYCE_RENDERER_API void GRender_RenderGizmo(void);
GRYCE_RENDERER_API void GRender_RenderGameView(void);
GRYCE_RENDERER_API void GRender_EndFrame(void);

GRYCE_RENDERER_API GTextureHandle GRender_GetViewportTexture(void);
GRYCE_RENDERER_API GTextureHandle GRender_GetGameViewTexture(void);
GRYCE_RENDERER_API int            GRender_GetViewportSize(int* out_w, int* out_h);
GRYCE_RENDERER_API int            GRender_GetGameViewSize(int* out_w, int* out_h);

GRYCE_RENDERER_API void  GRender_SetVSync(bool enabled);

// ---------------------------------------------------------------------------
// 帧率上限（"锁帧"）—— 与 VSync 互补的第二道限速
//
//   VSync 由交换链锁在显示器刷新率上（60/144/240 取决于显示器，不可调）；
//   帧率上限是软件节流：目标帧率任意可调，且不引入撕裂之外的行为差异。
//   fps <= 0 表示不限制（默认）。两者可以同时开启 —— 谁更严格谁生效。
//
//   实现位置在 GRender_EndFrame（提交/present 之后按需补睡到目标帧间隔），
//   因此同步模式与异步模式都生效，且不影响引擎内部的帧并行度。
// ---------------------------------------------------------------------------
GRYCE_RENDERER_API void  GRender_SetFrameRateLimit(int fps);
GRYCE_RENDERER_API int   GRender_GetFrameRateLimit(void);

// ---------------------------------------------------------------------------
// 帧统计读数（调试 UI / 自动化验证用）
//   GRender_GetFPS()            平滑后的帧率
//   GRender_GetFrameTimeMs()    最近一帧的墙钟帧时间（ms）
//   GRender_GetGPUFrameTimeMs() GPU 总耗时（摊到每帧；需先开启 GPU 计时，见
//                               GRender_SetGPUProfiling），未开启返回 0
// ---------------------------------------------------------------------------
GRYCE_RENDERER_API float  GRender_GetFPS(void);
GRYCE_RENDERER_API float  GRender_GetFrameTimeMs(void);
GRYCE_RENDERER_API float  GRender_GetGPUFrameTimeMs(void);

// ---------------------------------------------------------------------------
// 调试 UI（左上角 FPS/帧时间叠加 + ImGui 调试窗口）
//
//   默认跟随 GRender_SetFPSOverlay：开着就在左上角画 FPS/帧时间读数，
//   并且在"宿主没有自己的 ImGui 上下文"时由引擎接管 ImGui，额外弹出一个调试窗口
//   （F1 显示/隐藏），里面有：垂直同步、帧率上限、省电模式、GPU 计时的热开关，
//   以及若干渲染开关（阴影 / GPU 剔除 / SSR / IBL 强度）。
//
//   宿主自己管 ImGui（例如编辑器）时引擎不会抢上下文，此时宿主可以在自己的
//   NewFrame/Render 之间调用 GRender_DrawDebugPanel() 把这套面板画出来。
// ---------------------------------------------------------------------------
GRYCE_RENDERER_API void  GRender_SetDebugUI(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsDebugUIActive(void);
// 在宿主自持 ImGui 上下文时调用（须处于 ImGui::NewFrame() 与 ImGui::Render() 之间）。
GRYCE_RENDERER_API void  GRender_DrawDebugPanel(void);

// 电源 / 省电模式：未插电（电池）时自动进入，极致压缩每帧工作以稳定高帧率。
// 默认 auto 开启；forced 可手动强制开(true)/关(false)。插电时行为不变（零回归）。
GRYCE_RENDERER_API bool  GRender_IsOnBattery();          // 即时探测是否未插交流电
GRYCE_RENDERER_API void  GRender_SetPowerSaverAuto(bool enabled);
GRYCE_RENDERER_API void  GRender_SetPowerSaverForced(bool forced);
GRYCE_RENDERER_API bool  GRender_IsPowerSaverActive();

// SSR 未命中回退强度（0..1）：
//   1 = 命中处用屏幕空间反射替换该像素原本的 IBL 镜面项，未命中处保留 IBL /
//       反射探针 —— 屏幕外方向也有反射，且不会"IBL + SSR"双重计能（推荐）；
//   0 = 旧行为（SSR 叠加在 IBL 之上）。
GRYCE_RENDERER_API void  GRender_SetSSREnvFallback(float strength);

// SSR 内部渲染分辨率缩放（0.25~1.0，默认 1.0）。0.5 表示光线步进/模糊按半分辨率
// 渲染、合成仍全分辨率；SSR 的 GPU 时间大致按 scale² 下降。
GRYCE_RENDERER_API void  GRender_SetSSRResolutionScale(float scale);

// 调试视图（0=关闭；1=albedo 2=法线 3=粗糙度 4=金属度 5=阴影因子 6=直接光
// 7=环境光 8=级联着色）。用于排查光照/阴影问题。
GRYCE_RENDERER_API void  GRender_SetDebugView(int mode);

// PCSS 接触硬化软阴影（默认关闭）。light_size 为光源立体角近似（世界单位），
// max_radius_texels 是搜索/滤波的最大纹素半径。开启后阴影边缘随遮挡距离变软，
// 接触处保持硬 —— 比单一 PCF 更接近真实阴影。
GRYCE_RENDERER_API void  GRender_SetPCSSEnabled(bool enabled);
GRYCE_RENDERER_API void  GRender_SetPCSSParams(float light_size, float max_radius_texels,
                                               float tap_scale);

// 低/中/高配画质预设：0=高 1=中 2=低（仅切换运行时质量开关：阴影尺寸/PCSS/SSR/SSAO/SSIL）
GRYCE_RENDERER_API void  GRender_SetQualityPreset(int preset);
// 动态分辨率（DRS，默认关闭）：按实测帧时间自适应缩放内部渲染分辨率以稳定帧率
GRYCE_RENDERER_API void  GRender_SetAdaptiveResolution(bool enabled);
GRYCE_RENDERER_API void  GRender_SetResolutionScale(float scale);
GRYCE_RENDERER_API void  GRender_SetDynamicResolutionTargetFPS(float fps);
GRYCE_RENDERER_API void  GRender_SetDynamicResolutionMinScale(float scale);
// GPU 视锥剔除 + 间接绘制（默认开启；后端不支持 compute 时自动回退 CPU 路径）
// 把逐实例剔除搬到 GPU：CPU 每帧只上传一次实例数据 + 一次 dispatch，剔除结果
// 直接进入间接绘制参数，不做 GPU→CPU 读回（避免同步停顿）。
GRYCE_RENDERER_API void  GRender_SetGPUCullingEnabled(bool enabled);
// 在视锥剔除之上叠加基于深度金字塔（HiZ）的遮挡剔除：被墙/地形挡住的实例不再
// 进入间接绘制。依赖 R32F 颜色附件（GL 需 EXT_color_buffer_float），不支持时
// 自动关闭（视锥剔除不受影响）。Vulkan 后端的 uv 翻转约定尚未在真机验证，
// 出现物体闪烁可用它关掉。
GRYCE_RENDERER_API void  GRender_SetOcclusionCullingEnabled(bool enabled);
GRYCE_RENDERER_API bool GRender_IsOcclusionCullingActive();
// 运行时探测结果：backend 是否真的具备 compute 能力且着色器编译成功。
GRYCE_RENDERER_API bool  GRender_IsGPUCullingAvailable();

// 阴影缓存（默认开启）：相机/光源/投射体静止时跳过每帧阴影重渲
GRYCE_RENDERER_API void  GRender_SetShadowCacheEnabled(bool enabled);
// 跨 mesh 实例化合批（默认开启）：把同 mesh + 同材质的不透明物体合并为单次 instanced draw
GRYCE_RENDERER_API void  GRender_SetInstancingBatching(bool enabled);

// 左上角白色 FPS 叠加（直接画在 2D overlay 层，不受 HDR/后处理影响）。
GRYCE_RENDERER_API void  GRender_SetFPSOverlay(bool enabled);

// SSR 调试视图：0 正常合成；1 原始反射颜色；2 命中覆盖度；3 场景 IBL 镜面项。
GRYCE_RENDERER_API void  GRender_SetSSRDebugView(int mode);

// ---------------------------------------------------------------------------
// GPU 分段计时（性能分析）
//   开启后引擎用硬件计时器（GL: GL_TIME_ELAPSED / Vulkan: vkCmdWriteTimestamp）
//   统计每个 pass 的实际 GPU 耗时，结果在帧槽复用时异步结算，不影响帧时间。
//   典型用法：GRender_SetGPUProfiling(1) → 渲染若干帧 → GRender_DumpGPUProfiling()。
//   GRender_DumpGPUProfiling() 只读取累计统计并打印，可在主线程安全调用。
// ---------------------------------------------------------------------------
GRYCE_RENDERER_API void  GRender_SetGPUProfiling(int enabled);
GRYCE_RENDERER_API int   GRender_GPUProfilingSupported(void);
GRYCE_RENDERER_API void  GRender_DumpGPUProfiling(void);
GRYCE_RENDERER_API int   GRender_SaveScreenshot(const char* path);
GRYCE_RENDERER_API void GRender_SetDisplayMode(const char* mode);

// 2D 场景编辑器模式：GRender_RenderWorld 只渲染 2D 画布
//（清屏 + 2D 覆盖层，不跑 3D 管线）。false = 3D 场景 + 2D 覆盖。
GRYCE_RENDERER_API void GRender_SetScene2D(bool enabled);

// --- Project Settings（渲染质量；大部分可运行时调整，阴影贴图尺寸/后端重启生效）---
GRYCE_RENDERER_API void  GRender_SetHDR(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsHDR(void);
// MSAA 采样数（前向 HDR pass；1 = 关闭，默认 4）。init 前调用直接生效；
// init 后调用同步重建 HDR 目标。实际上限由后端能力协商（GL_MAX_SAMPLES /
// framebufferColorSampleCounts / D3D12 CheckFeatureSupport），创建失败自动降级。
GRYCE_RENDERER_API void  GRender_SetMSAASamples(int samples);
GRYCE_RENDERER_API int   GRender_GetMSAASamples(void);
GRYCE_RENDERER_API void  GRender_SetToneMapMode(int mode);   // 0=None, 1=Reinhard, 2=ACES
GRYCE_RENDERER_API int   GRender_GetToneMapMode(void);
GRYCE_RENDERER_API void  GRender_SetExposure(float exposure);
GRYCE_RENDERER_API float GRender_GetExposure(void);
GRYCE_RENDERER_API void  GRender_SetShadowEnabled(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsShadowEnabled(void);
GRYCE_RENDERER_API void  GRender_SetShadowMapSize(int size); // 重启生效
GRYCE_RENDERER_API int   GRender_GetShadowMapSize(void);
GRYCE_RENDERER_API void  GRender_SetAmbient(float r, float g, float b);
GRYCE_RENDERER_API void  GRender_GetAmbient(float* r, float* g, float* b);
GRYCE_RENDERER_API void  GRender_SetIBLIntensity(float intensity);
GRYCE_RENDERER_API float GRender_GetIBLIntensity(void);

// 默认程序化环境（天空 + IBL）：没有环境贴图时，metallic=1 的材质没有任何
// 可反射内容、只剩太阳高光，画面看起来像"材质坏了"。开启后引擎会生成一张
// 内置 equirect 天空（天顶冷蓝 → 地平线暖白 + 太阳亮斑）并派生 IBL，
// 同时作为天空盒背景。项目自带 HDR 环境时应保持关闭。
GRYCE_RENDERER_API void GRender_SetDefaultEnvironment(bool enabled);
GRYCE_RENDERER_API bool GRender_IsDefaultEnvironmentEnabled(void);

// --- 屏幕空间效果（光追/间接光/环境光遮蔽）---
// SSR/SSIL 需要每像素法线+粗糙度：管线会在开启后自动跑一遍 depth+normal
// 预通道；若该通道不可用（着色器缺失），SSR 会被安全跳过。
// 三者默认关闭；SSAO 只需深度，最轻量；SSR 最贵。
GRYCE_RENDERER_API void  GRender_SetSSAO(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsSSAOEnabled(void);
GRYCE_RENDERER_API void  GRender_SetSSR(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsSSREnabled(void);
GRYCE_RENDERER_API void  GRender_SetSSIL(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsSSILEnabled(void);
// Scene View 网格线：编辑器辅助显示，默认关闭。游戏运行时/打包版本不要打开，
// 只有编辑器视口需要地面尺度参照时才由编辑器显式开启。
GRYCE_RENDERER_API void  GRender_SetGrid(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsGridEnabled(void);
// SSR 质量参数：max_steps 光线步进上限，max_roughness 参与反射的粗糙度上限，
// thickness 屏幕空间厚度（世界单位），bilateral 反射去噪强度 0~1。
GRYCE_RENDERER_API void  GRender_SetSSRParams(float max_steps, float max_roughness,
                                              float thickness, float bilateral);

// --- 材质预设（表见 render/material_presets.h）---
GRYCE_RENDERER_API int   GRender_GetMaterialPresetCount(void);
// 分别取稳定标识与显示名；写入 out_buf（含终止符），成功返回写入长度，失败返回 -1。
GRYCE_RENDERER_API int   GRender_GetMaterialPresetName(int index, char* out_buf, int buf_size);
GRYCE_RENDERER_API int   GRender_GetMaterialPresetLabel(int index, char* out_buf, int buf_size);

// Hot backend switch: the request is applied on the render thread at the next
// GRender_BeginFrame (renderer + embedded window are recreated).
GRYCE_RENDERER_API void  GRender_RequestBackend(GRenderAPI api);

// Hot reload: rebuilds the render pipeline (shaders/FBOs/post-process targets)
// in place at the next GRender_BeginFrame, preserving current configuration.
GRYCE_RENDERER_API int   GRender_RebuildPipeline(void);

// Recovers the embedded render surface after the host HWND hid or destroyed
// it (e.g. switching to the code-editor tab and back): forces a same-backend
// recreate of the GLFW child window + render context + swapchain at the next
// GRender_BeginFrame.
GRYCE_RENDERER_API void  GRender_RequestSurfaceRecreate(void);

#ifdef __cplusplus
}
#endif

#endif
