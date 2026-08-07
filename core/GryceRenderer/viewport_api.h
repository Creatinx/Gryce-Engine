#ifndef GRYCE_VIEWPORT_API_H
#define GRYCE_VIEWPORT_API_H

#include "GryceCore/types.h"

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

GRYCE_RENDERER_API void GGameView_SetSize(int w, int h);
GRYCE_RENDERER_API void GGameView_GetSize(int* out_w, int* out_h);
GRYCE_RENDERER_API void GGameView_SetCamera(GEntityHandle camera_entity);

#ifdef __cplusplus
}
#endif

#endif
