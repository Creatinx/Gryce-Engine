#ifndef GRYCE_ASSET_API_H
#define GRYCE_ASSET_API_H

#include "types.h"

#ifdef _WIN32
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_CORE_API __declspec(dllexport)
    #else
        #define GRYCE_CORE_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GryceCore.lib")
        #endif
    #endif
#else
    #define GRYCE_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

GRYCE_CORE_API GAssetHandle GAsset_Import(const char* source_path);
GRYCE_CORE_API GAssetHandle GAsset_Load(const char* path);
GRYCE_CORE_API int          GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size);
GRYCE_CORE_API void         GAsset_Unload(GAssetHandle handle);

#ifdef __cplusplus
}
#endif

#endif
