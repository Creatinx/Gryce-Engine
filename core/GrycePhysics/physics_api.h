#ifndef GRYCE_PHYSICS_API_H
#define GRYCE_PHYSICS_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_PHYSICS_BUILDING
        #define GRYCE_PHYSICS_API __declspec(dllexport)
    #else
        #define GRYCE_PHYSICS_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GrycePhysics.lib")
        #endif
    #endif
#else
    #define GRYCE_PHYSICS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

GRYCE_PHYSICS_API int  GPhysics_Init(GPhysicsBackend backend);
GRYCE_PHYSICS_API void GPhysics_Shutdown(void);

GRYCE_PHYSICS_API void GPhysics_SetGravity(const GVec3* gravity);
GRYCE_PHYSICS_API void GPhysics_Step(float dt, int substeps);

GRYCE_PHYSICS_API GBodyHandle GPhysics_CreateBody(GEntityHandle entity, bool is_static);
GRYCE_PHYSICS_API void GPhysics_DestroyBody(GBodyHandle body);
GRYCE_PHYSICS_API void GPhysics_SetBodyTransform(GBodyHandle body, const GVec3* pos, const GQuat* rot);
GRYCE_PHYSICS_API void GPhysics_GetBodyTransform(GBodyHandle body, GVec3* out_pos, GQuat* out_rot);
GRYCE_PHYSICS_API void GPhysics_AddForce(GBodyHandle body, const GVec3* force);
GRYCE_PHYSICS_API void GPhysics_AddImpulse(GBodyHandle body, const GVec3* impulse);

GRYCE_PHYSICS_API bool GPhysics_Raycast(const GVec3* origin, const GVec3* dir, float max_dist,
                                         GVec3* out_hit_point, GVec3* out_hit_normal,
                                         GEntityHandle* out_entity);

#ifdef __cplusplus
}
#endif

#endif
