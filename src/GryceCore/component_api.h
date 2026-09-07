#ifndef GRYCE_COMPONENT_API_H
#define GRYCE_COMPONENT_API_H

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

GRYCE_CORE_API int GComponent_GetCount(GEntityHandle entity);
GRYCE_CORE_API int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash);
GRYCE_CORE_API int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size);

GRYCE_CORE_API int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash);
GRYCE_CORE_API int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,
                                               char* out_name, int name_buf_size,
                                               int* out_type, int* out_size);
GRYCE_CORE_API int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                           void* out_value, int value_size);
GRYCE_CORE_API int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                           const void* value, int value_size);

// ---------------------------------------------------------------------------
// Tilemap 瓦片数据（tiles 向量不在反射标量范围内，提供专用数组接口）
// ---------------------------------------------------------------------------
// 读取瓦片数组到 out_tiles（最多 max_count 个）。返回实际写入的数量；失败返回 -1。
GRYCE_CORE_API int GComponent_TilemapGetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                                              int* out_tiles, int max_count);
// 写入瓦片数组（count 为 -1 时清空）。返回 0 成功，-1 失败。
GRYCE_CORE_API int GComponent_TilemapSetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                                              const int* tiles, int count);

GRYCE_CORE_API int GComponent_AddComponent(GEntityHandle entity, uint64_t comp_type_hash);
GRYCE_CORE_API int GComponent_RemoveComponent(GEntityHandle entity, uint64_t comp_type_hash);

// 给实体（无则加）MeshRenderer 组件并设置网格路径与材质颜色。
// mesh_path 可为空（仅改颜色，要求已存在 MeshRenderer）。
// 直接操作内部 MeshRenderer::material（嵌套对象，反射标量无法触达）。
// 返回 0 成功，-1 失败（实体不存在 / 场景未初始化）。
GRYCE_CORE_API int GComponent_MeshSetMaterial(GEntityHandle entity, const char* mesh_path,
                                              float r, float g, float b,
                                              float roughness, float metallic);

// 读取实体 MeshRenderer 组件的网格路径到 out_buf（可空终止）。返回 0 成功，-1 失败
//（实体不存在 / 无 MeshRenderer / 路径为空）。
GRYCE_CORE_API int GComponent_MeshGetPath(GEntityHandle entity, char* out_buf, int buf_size);

// 读取实体 MeshRenderer 嵌套材质的 PBR 参数（alb/rough/metal）。返回 0 成功，-1 失败
//（实体不存在 / 无 MeshRenderer / 材质未初始化）。
GRYCE_CORE_API int GComponent_MeshGetMaterial(GEntityHandle entity,
                                              float* r, float* g, float* b,
                                              float* roughness, float* metallic);

GRYCE_CORE_API int GComponent_GetRegisteredTypeCount(void);
GRYCE_CORE_API int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size);
GRYCE_CORE_API int GComponent_GetRegisteredTypeCategory(int index, char* out_category, int category_buf_size);

#ifdef __cplusplus
}
#endif

#endif
