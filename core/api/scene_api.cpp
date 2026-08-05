#include "GryceCore/scene_api.h"

extern "C" {

int GScene_Load(const char* path) { (void)path; return -1; }
int GScene_Save(const char* path) { (void)path; return -1; }
int GScene_GetCurrentPath(char* out_buf, int buf_size) { (void)out_buf; (void)buf_size; return 0; }
int GScene_New(void) { return 0; }

} // extern "C"
