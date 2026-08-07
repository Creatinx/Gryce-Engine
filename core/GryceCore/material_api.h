#ifndef GRYCE_MATERIAL_API_H
#define GRYCE_MATERIAL_API_H

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

// 材质字段枚举（MeshRenderer / SkinnedMeshRenderer 的 PBR 材质）。
typedef enum {
    GMAT_ALBEDO_COLOR = 0,     // float[3]  RGB
    GMAT_ROUGHNESS,            // float
    GMAT_METALLIC,             // float
    GMAT_AO,                   // float
    GMAT_EMISSIVE_COLOR,       // float[3]  RGB
    GMAT_OPACITY,              // float
    GMAT_BLEND_MODE,           // int  (0=Opaque, 1=Blend)
    GMAT_TWO_SIDED,            // bool
    GMAT_UV_SCALE,             // float[2]
    GMAT_UV_OFFSET,            // float[2]
    GMAT_ALBEDO_MAP_PATH,      // string
    GMAT_NORMAL_MAP_PATH,      // string
    GMAT_ROUGHNESS_MAP_PATH,   // string
    GMAT_METALLIC_MAP_PATH,    // string
    GMAT_AO_MAP_PATH,          // string
    GMAT_EMISSIVE_MAP_PATH,    // string
    GMAT_USE_ALBEDO_MAP,       // bool
    GMAT_USE_NORMAL_MAP,       // bool
    GMAT_USE_ROUGHNESS_MAP,    // bool
    GMAT_USE_METALLIC_MAP,     // bool
    GMAT_USE_AO_MAP,           // bool
    GMAT_USE_EMISSIVE_MAP,     // bool
    GMAT_COUNT
} GMaterialField;

// 读取材质字段。
// 标量/向量字段写入 out_floats（容量 float_capacity）；字符串字段写入 out_str。
// 返回 0 成功，-1 失败（实体/组件/字段无效）。
GRYCE_CORE_API int GMaterial_GetField(GEntityHandle entity, uint64_t comp_type_hash,
                                      int field, float* out_floats, int float_capacity,
                                      char* out_str, int str_capacity);

// 写入材质字段。字符串字段传入 in_str，其余传入 in_floats（float_count 个）。
// 修改贴图路径/use 标志时会把 material 标记为 textures_dirty，
// 渲染线程下一帧自动重新上传贴图。
GRYCE_CORE_API int GMaterial_SetField(GEntityHandle entity, uint64_t comp_type_hash,
                                      int field, const float* in_floats, int float_count,
                                      const char* in_str);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_MATERIAL_API_H
