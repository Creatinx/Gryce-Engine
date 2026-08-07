#ifndef GRYCE_SCENE_API_H
#define GRYCE_SCENE_API_H

#include "types.h"

#ifdef _WIN32
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_CORE_API __declspec(dllexport)
    #else
        #define GRYCE_CORE_API __declspec(dllimport)
        #ifdef _MSC_VER
            #ifdef _DEBUG
                #pragma comment(lib, "GryceCored.lib")
            #else
                #pragma comment(lib, "GryceCore.lib")
            #endif
        #endif
    #endif
#else
    #define GRYCE_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

GRYCE_CORE_API int GScene_Load(const char* path);
GRYCE_CORE_API int GScene_Save(const char* path);
GRYCE_CORE_API int GScene_GetCurrentPath(char* out_buf, int buf_size);
GRYCE_CORE_API int GScene_New(void);

#ifdef __cplusplus
}
#endif

#endif
