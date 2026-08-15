#ifndef GRYCE_INPUT_API_H
#define GRYCE_INPUT_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_PLATFORM_BUILDING
        #define GRYCE_PLATFORM_API __declspec(dllexport)
    #else
        #define GRYCE_PLATFORM_API __declspec(dllimport)
        #ifdef _MSC_VER
            #ifdef _DEBUG
                #pragma comment(lib, "GrycePlatformd.lib")
            #else
                #pragma comment(lib, "GrycePlatform.lib")
            #endif
        #endif
    #endif
#else
    #define GRYCE_PLATFORM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

GRYCE_PLATFORM_API void GInput_InjectKey(int key_code, GInputAction action);
GRYCE_PLATFORM_API void GInput_InjectMouseMove(float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseButton(int button, GInputAction action, float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseScroll(float delta_x, float delta_y);
// 清除鼠标增量累计与绝对位置基线：场景/游戏视图切换或重新锁定指针后调用，
// 下一次 GInput_InjectMouseMove 会重新基线（不产生假 delta）。
GRYCE_PLATFORM_API void GInput_ResetMouseBaseline(void);

GRYCE_PLATFORM_API bool GInput_IsKeyPressed(int key_code);
GRYCE_PLATFORM_API bool GInput_IsKeyHeld(int key_code);
GRYCE_PLATFORM_API bool GInput_IsMouseButtonPressed(int button);
GRYCE_PLATFORM_API void GInput_GetMousePosition(float* out_x, float* out_y);

// 锁定/解锁鼠标（GLFW_CURSOR_DISABLED）。锁定后光标隐藏并持续报告移动增量，
// 供 FPS 视角旋转使用。独立宿主与编辑器注入路径均可用。
GRYCE_PLATFORM_API void GInput_SetMouseLocked(bool locked);

// 返回本帧累计的鼠标移动增量（像素）。独立宿主轮询路径使用。
GRYCE_PLATFORM_API void GInput_GetMouseDelta(float* out_dx, float* out_dy);

// 独立游戏宿主用：把平台层轮询到的 GLFW 键盘/鼠标状态同步进 Core 的输入
// 状态（engine.input 查询的数据源）。编辑器走 GInput_Inject* 事件注入，
// 不需要调用本函数。
GRYCE_PLATFORM_API void GInput_SyncToCore(void);

#ifdef __cplusplus
}
#endif

#endif
