#include "GryceCore/component_api.h"

extern "C" {

int GComponent_GetCount(GEntityHandle entity) { (void)entity; return 0; }
int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash) { (void)entity; (void)index; (void)out_hash; return -1; }
int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size) { (void)entity; (void)index; (void)out_buf; (void)buf_size; return -1; }

int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash) { (void)entity; (void)comp_type_hash; return 0; }
int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,
                               char* out_name, int name_buf_size,
                               int* out_type, int* out_size) {
    (void)entity; (void)comp_type_hash; (void)prop_index; (void)out_name; (void)name_buf_size;
    (void)out_type; (void)out_size; return -1;
}
int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           void* out_value, int value_size) {
    (void)entity; (void)comp_type_hash; (void)prop_name; (void)out_value; (void)value_size; return -1;
}
int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           const void* value, int value_size) {
    (void)entity; (void)comp_type_hash; (void)prop_name; (void)value; (void)value_size; return -1;
}

int GComponent_GetRegisteredTypeCount(void) { return 0; }
int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size) {
    (void)index; (void)out_hash; (void)out_name; (void)name_buf_size; return -1;
}

} // extern "C"
