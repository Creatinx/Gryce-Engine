#ifndef GRYCE_VIEWPORT_API_H
#define GRYCE_VIEWPORT_API_H

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

GRYCE_RENDERER_API void GViewport_SetSize(int w, int h);
GRYCE_RENDERER_API void GViewport_GetSize(int* out_w, int* out_h);
GRYCE_RENDERER_API void GViewport_SetCamera(GEntityHandle camera_entity);
GRYCE_RENDERER_API GEntityHandle GViewport_GetCamera(void);

// 设置编辑器相机参数（直接使用，不依赖场景中的 Camera 实体）
GRYCE_RENDERER_API void GViewport_SetEditorCamera(
    float pos_x, float pos_y, float pos_z,
    float yaw, float pitch,
    float fov, float near_plane, float far_plane);

// 用四元数设置编辑器相机朝向。
//
// 为什么不只用上面的 yaw/pitch：yaw/pitch 是"某套约定"下的参数，宿主与引擎
// 之间一旦约定不一致（正负号、零度朝哪个轴），画面与拾取就会差 90 度或上下
// 颠倒，而且两边各自都能自圆其说，极难定位。四元数是朝向本身，不存在约定
// 问题：宿主用同一份四元数算射线，就一定与渲染一致。
//
// 语义：旋转应用在相机默认朝向 -Z 上（与场景 Camera 组件一致）。
// @owner engine —— 任何需要"任意朝向相机"的宿主都用得上，不只是编辑器
GRYCE_RENDERER_API void GViewport_SetEditorTransform(
    float pos_x, float pos_y, float pos_z,
    float qx, float qy, float qz, float qw,
    float fov, float near_plane, float far_plane);

// 视口在窗口里的屏幕原点（左上角，像素；默认 0,0）。
//
// 为什么需要它：Gizmo 是画在 ImGui 的前景 draw list 上的，而 ImGui 的坐标
// 是窗口绝对坐标；视口贴图却只知道自己内部是 vw×vh。宿主要么告诉引擎
// "视口在窗口的哪个位置"，要么就只能把 3D 视口铺满整个窗口。
//
// 契约：设置原点之后，GRender_GizmoHitTest 的入参是**窗口绝对坐标**
// （与鼠标、与 ImGui 一致）。不设置时原点为 0，等价于"视口铺满窗口"，
// 与旧行为完全一致。
GRYCE_RENDERER_API void GViewport_SetScreenOrigin(float x, float y);
GRYCE_RENDERER_API void GViewport_GetScreenOrigin(float* out_x, float* out_y);

GRYCE_RENDERER_API void GGameView_SetSize(int w, int h);
GRYCE_RENDERER_API void GGameView_GetSize(int* out_w, int* out_h);
GRYCE_RENDERER_API void GGameView_SetCamera(GEntityHandle camera_entity);

#ifdef __cplusplus
}
#endif

#endif
