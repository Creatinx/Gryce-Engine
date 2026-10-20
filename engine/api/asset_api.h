#ifndef GRYCE_ASSET_API_H
#define GRYCE_ASSET_API_H

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

GRYCE_CORE_API GAssetHandle GAsset_Import(const char* source_path);
GRYCE_CORE_API GAssetHandle GAsset_Load(const char* path);
GRYCE_CORE_API int          GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size);
GRYCE_CORE_API void         GAsset_Unload(GAssetHandle handle);

// ---------------------------------------------------------------------------
// 资产侧车文件（<asset>.meta）
//
// 编辑器需要"资产的身份"独立于它的路径：层级里的引用、材质槽、场景片段都按
// guid 指向资产，重命名/移动文件不会断链。侧车还保存导入设置（缩放、上轴、
// 材质开关、粗糙度倍率），下次导入按它生效。
//
// 侧车缺失时 GAsset_EnsureMeta 会按源文件生成一份并写盘（目录只读时只返回
// 内存中的结果），因此调用方不需要额外的"先创建"步骤。
// ---------------------------------------------------------------------------
typedef struct {
    char  guid[40];            // 32 位十六进制 + 结束符
    char  source_path[512];    // 源资产路径（诊断用）
    char  importer[32];        // "mesh" / "texture" / "skinned" / "audio"
    float scale;               // 源单位 → 米
    int   up_axis;             // 0=X-up，1=Y-up，2=Z-up
    int   generate_lod;        // 1 = 导入后生成 LOD 链
    int   import_materials;    // 1 = 采用文件自带材质
    float roughness_scale;     // 叠在推导结果上的粗糙度倍率
    int   version;             // 侧车格式版本
} GAssetMetaInfo;

// 读侧车（不存在或损坏返回 0，不改动 out）。
GRYCE_CORE_API int  GAsset_ReadMeta(const char* asset_path, GAssetMetaInfo* out);
// 读侧车；不存在则生成（含新 guid 与源文件指纹）并写盘。
GRYCE_CORE_API int  GAsset_EnsureMeta(const char* asset_path, GAssetMetaInfo* out);
// 写侧车（缺失字段用当前引擎默认值补齐）。返回 1/0。
GRYCE_CORE_API int  GAsset_WriteMeta(const char* asset_path, const GAssetMetaInfo* info);
// 源文件指纹与侧车记录不一致时为 1 —— 编辑器据此提示"资源已变更，是否重新导入"。
GRYCE_CORE_API int  GAsset_IsMetaStale(const char* asset_path);

#ifdef __cplusplus
}
#endif

#endif
