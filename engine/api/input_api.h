#ifndef GRYCE_INPUT_API_H
#define GRYCE_INPUT_API_H

#include "api/types.h"
#include "api/window_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Input injection (external HWND mode; OS events are pushed in by the host).
GRYCE_PLATFORM_API void GInput_InjectKey(int key_code, GInputAction action);
GRYCE_PLATFORM_API void GInput_InjectMouseMove(float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseButton(int button, GInputAction action, float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseScroll(float delta_x, float delta_y);
GRYCE_PLATFORM_API void GInput_ResetMouseBaseline(void);

// Query helpers.
GRYCE_PLATFORM_API bool GInput_IsKeyPressed(int key_code);
GRYCE_PLATFORM_API bool GInput_IsKeyHeld(int key_code);
GRYCE_PLATFORM_API bool GInput_IsMouseButtonPressed(int button);
GRYCE_PLATFORM_API bool GInput_IsMouseButtonHeld(int button);
GRYCE_PLATFORM_API bool GInput_IsMouseButtonReleased(int button);
GRYCE_PLATFORM_API void GInput_GetMousePosition(float* out_x, float* out_y);
GRYCE_PLATFORM_API void GInput_GetMouseDelta(float* out_dx, float* out_dy);
GRYCE_PLATFORM_API void GInput_SetMouseLocked(bool locked);

// 文本输入（Unicode 码点）：把本帧积攒的字符取走，返回实际条数。
// 引擎内置 UI 的输入框用它；GLFW 模式下由窗口的字符回调积累，
// 外部 HWND 模式由宿主用 GInput_InjectChar 注入。
GRYCE_PLATFORM_API int  GInput_PullChars(unsigned int* out, int cap);
GRYCE_PLATFORM_API void GInput_InjectChar(unsigned int codepoint);

// 滚轮：本帧累积增量，**取走即清零**（滚轮是事件，读两次不该翻倍）。
// 正 y = 向上滚。宿主没给窗口时由 GInput_InjectMouseScroll 注入。
GRYCE_PLATFORM_API void GInput_GetWheel(float* out_dx, float* out_dy);

// 输入法预编辑串（UTF-8，组合过程中的下划线文本）。
// GLFW 不暴露 IME 的 preedit（只把最终提交的字符送到 char 回调），所以组合
// 串由宿主注入：外部 HWND 模式接 IMM32，或编辑器自己的输入法桥。空串 = 无
// 组合。内置 UI 的输入框会把它画在插入点处并加下划线。
GRYCE_PLATFORM_API void GInput_InjectPreedit(const char* utf8_text);
GRYCE_PLATFORM_API int  GInput_GetPreedit(char* out_buf, int buf_size);

// Push changed input state into the engine command ring buffer.
GRYCE_PLATFORM_API void GInput_SyncToCore(void);

#ifdef __cplusplus
}
#endif

#endif