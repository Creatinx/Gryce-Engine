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
GRYCE_RENDERER_API int   GRender_SaveScreenshot(const char* path);
GRYCE_RENDERER_API void GRender_SetDisplayMode(const char* mode);

// 2D 场景编辑器模式：GRender_RenderWorld 只渲染 2D 画布
//（清屏 + 2D 覆盖层，不跑 3D 管线）。false = 3D 场景 + 2D 覆盖。
GRYCE_RENDERER_API void GRender_SetScene2D(bool enabled);

// --- Project Settings（渲染质量；大部分可运行时调整，阴影贴图尺寸/后端重启生效）---
GRYCE_RENDERER_API void  GRender_SetHDR(bool enabled);
GRYCE_RENDERER_API bool  GRender_IsHDR(void);
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
