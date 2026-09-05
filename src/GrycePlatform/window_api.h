#ifndef GRYCE_WINDOW_API_H
#define GRYCE_WINDOW_API_H

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

GRYCE_PLATFORM_API int  GWindow_InitExternal(GWindowHandle hwnd, int w, int h);
GRYCE_PLATFORM_API int  GWindow_InitExternalEx(GWindowHandle hwnd, int w, int h, GRenderAPI api);
GRYCE_PLATFORM_API int  GWindow_Create(const char* title, int w, int h, GWindowMode mode);
GRYCE_PLATFORM_API int  GWindow_RecreateClientApi(GRenderAPI api);
GRYCE_PLATFORM_API void GWindow_Destroy(void);
GRYCE_PLATFORM_API bool GWindow_IsValid(void);

GRYCE_PLATFORM_API void GWindow_GetSize(int* out_w, int* out_h);
GRYCE_PLATFORM_API void GWindow_GetFramebufferSize(int* out_w, int* out_h);
GRYCE_PLATFORM_API void GWindow_SetSize(int w, int h);
GRYCE_PLATFORM_API GWindowHandle GWindow_GetNativeHandle(void);
GRYCE_PLATFORM_API GWindowHandle GWindow_GetRenderHandle(void);
GRYCE_PLATFORM_API bool GWindow_ShouldClose(void);
GRYCE_PLATFORM_API void GWindow_PollEvents(void);
GRYCE_PLATFORM_API void GWindow_SwapBuffers(void);

// GL context ownership lives inside the core (single GLFW instance). The
// editor render thread uses these to take/release/pace the context so it never
// needs to P/Invoke GLFW directly (which would risk binding a second,
// uninitialized GLFW copy in mixed toolchain layouts).
GRYCE_PLATFORM_API void GWindow_MakeContextCurrent(void);
GRYCE_PLATFORM_API void GWindow_ReleaseContext(void);
GRYCE_PLATFORM_API void GWindow_SetSwapInterval(int interval);

#ifdef __cplusplus
}
#endif

#endif
