#include "GryceCore/asset_api.h"

extern "C" {

GAssetHandle GAsset_Import(const char* source_path) { (void)source_path; return 0; }
GAssetHandle GAsset_Load(const char* path) { (void)path; return 0; }
int GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size) { (void)handle; (void)out_buf; (void)buf_size; return 0; }
void GAsset_Unload(GAssetHandle handle) { (void)handle; }

} // extern "C"
