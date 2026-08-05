#include "GrycePhysics/physics_api.h"

extern "C" {

int GPhysics_Init(GPhysicsBackend backend) { (void)backend; return 0; }
void GPhysics_Shutdown(void) {}

void GPhysics_SetGravity(const GVec3* gravity) { (void)gravity; }
void GPhysics_Step(float dt, int substeps) { (void)dt; (void)substeps; }

GBodyHandle GPhysics_CreateBody(GEntityHandle entity, bool is_static) { (void)entity; (void)is_static; return 0; }
void GPhysics_DestroyBody(GBodyHandle body) { (void)body; }
void GPhysics_SetBodyTransform(GBodyHandle body, const GVec3* pos, const GQuat* rot) { (void)body; (void)pos; (void)rot; }
void GPhysics_GetBodyTransform(GBodyHandle body, GVec3* out_pos, GQuat* out_rot) { (void)body; (void)out_pos; (void)out_rot; }
void GPhysics_AddForce(GBodyHandle body, const GVec3* force) { (void)body; (void)force; }
void GPhysics_AddImpulse(GBodyHandle body, const GVec3* impulse) { (void)body; (void)impulse; }

bool GPhysics_Raycast(const GVec3* origin, const GVec3* dir, float max_dist,
                       GVec3* out_hit_point, GVec3* out_hit_normal,
                       GEntityHandle* out_entity) {
    (void)origin; (void)dir; (void)max_dist; (void)out_hit_point; (void)out_hit_normal; (void)out_entity;
    return false;
}

} // extern "C"
