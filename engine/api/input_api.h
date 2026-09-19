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
GRYCE_PLATFORM_API void GInput_GetMousePosition(float* out_x, float* out_y);
GRYCE_PLATFORM_API void GInput_GetMouseDelta(float* out_dx, float* out_dy);
GRYCE_PLATFORM_API void GInput_SetMouseLocked(bool locked);

// Push changed input state into the engine command ring buffer.
GRYCE_PLATFORM_API void GInput_SyncToCore(void);

#ifdef __cplusplus
}
#endif

#endif