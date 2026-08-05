#include "GryceCore/entity_api.h"

extern "C" {

int GEntity_GetCount(void) { return 0; }
GEntityHandle GEntity_GetAt(int index) { (void)index; return 0; }
int GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size) { (void)entity; (void)out_buf; (void)buf_size; return 0; }
int GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size) { (void)entity; (void)out_buf; (void)buf_size; return 0; }
GEntityHandle GEntity_GetParent(GEntityHandle entity) { (void)entity; return 0; }
int GEntity_GetChildCount(GEntityHandle entity) { (void)entity; return 0; }
GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index) { (void)entity; (void)index; return 0; }
int GEntity_GetSiblingIndex(GEntityHandle entity) { (void)entity; return -1; }

GEntityHandle GEntity_GetSelected(void) { return 0; }

int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos) { (void)entity; (void)out_pos; return -1; }
int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot) { (void)entity; (void)out_rot; return -1; }
int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale) { (void)entity; (void)out_scale; return -1; }
int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos) { (void)entity; (void)out_pos; return -1; }
int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot) { (void)entity; (void)out_rot; return -1; }
int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale) { (void)entity; (void)out_scale; return -1; }

} // extern "C"
