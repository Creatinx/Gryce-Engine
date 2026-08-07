#ifndef GRYCE_ENTITY_API_H
#define GRYCE_ENTITY_API_H

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

GRYCE_CORE_API int           GEntity_GetCount(void);
GRYCE_CORE_API GEntityHandle GEntity_GetAt(int index);
GRYCE_CORE_API int           GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API int           GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API GEntityHandle GEntity_GetParent(GEntityHandle entity);
GRYCE_CORE_API int           GEntity_GetChildCount(GEntityHandle entity);
GRYCE_CORE_API GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index);
GRYCE_CORE_API int           GEntity_GetSiblingIndex(GEntityHandle entity);

GRYCE_CORE_API GEntityHandle GEntity_GetSelected(void);

GRYCE_CORE_API int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale);
GRYCE_CORE_API int GEntity_SetLocalPosition(GEntityHandle entity, const GVec3* pos);
GRYCE_CORE_API int GEntity_SetLocalRotation(GEntityHandle entity, const GQuat* rot);
GRYCE_CORE_API int GEntity_SetLocalScale(GEntityHandle entity, const GVec3* scale);
GRYCE_CORE_API int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale);

#ifdef __cplusplus
}
#endif

#endif
