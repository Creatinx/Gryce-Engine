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

// 屏幕拾取：从 camera_entity 的 Transform + Camera 组件发射射线，对带
// MeshRenderer / SkinnedMeshRenderer 的实体做世界 AABB 求交，返回最近命中
// 实体句柄（未命中返回 0）。
GRYCE_CORE_API GEntityHandle GScene_PickScreen(float sx, float sy,
                                               int viewport_w, int viewport_h,
                                               GEntityHandle camera_entity);

// 世界空间射线拾取（direction 无需归一化）。max_dist <= 0 表示不限距离。
GRYCE_CORE_API GEntityHandle GScene_PickRay(const GVec3* origin,
                                            const GVec3* direction,
                                            float max_dist);

#ifdef __cplusplus
}
#endif

#endif
