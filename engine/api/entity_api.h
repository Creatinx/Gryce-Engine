#ifndef GRYCE_ENTITY_API_H
#define GRYCE_ENTITY_API_H

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

GRYCE_CORE_API int           GEntity_GetCount(void);
GRYCE_CORE_API GEntityHandle GEntity_GetAt(int index);
GRYCE_CORE_API int           GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API int           GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API GEntityHandle GEntity_GetParent(GEntityHandle entity);
GRYCE_CORE_API int           GEntity_GetChildCount(GEntityHandle entity);
GRYCE_CORE_API GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index);
GRYCE_CORE_API int           GEntity_GetSiblingIndex(GEntityHandle entity);

GRYCE_CORE_API GEntityHandle GEntity_GetSelected(void);

GRYCE_CORE_API int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale);
GRYCE_CORE_API int GEntity_SetLocalPosition(GEntityHandle entity, const GVec3* pos);
GRYCE_CORE_API int GEntity_SetLocalRotation(GEntityHandle entity, const GQuat* rot);
GRYCE_CORE_API int GEntity_SetLocalScale(GEntityHandle entity, const GVec3* scale);
GRYCE_CORE_API int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale);

// 导出实体及其全部子孙为 JSON（扁平 entities 数组，根 parent=null）。
// 用于 Undo/Redo 恢复、剪贴板复制粘贴与 Prefab 创建。
// 返回写入字节数；缓冲区不足返回 -1。
GRYCE_CORE_API int GEntity_ExportJson(GEntityHandle entity, char* out_buf, int buf_size);

// 从 GEntity_ExportJson 产生的 JSON 导入实体子树到指定父级
//（parent=0 表示场景根）。生成全新 UUID / EntityID，返回新子树根句柄，
// 失败返回 0。
GRYCE_CORE_API GEntityHandle GEntity_ImportJson(const char* json, GEntityHandle parent_handle);

// 把实体子树保存为 Prefab 文件（.gesc / .geprefab，root 的父引用被截断）。
// 返回 0 成功，-1 失败。
GRYCE_CORE_API int GEntity_SaveAsPrefab(GEntityHandle entity, const char* path);

// 实例化 Prefab 到场景（自动挂 PrefabInstance），可选挂到指定父级下。
// 返回新实例根句柄（失败返回 0）。
GRYCE_CORE_API GEntityHandle GEntity_CreatePrefabInstance(const char* prefab_path,
                                                          GEntityHandle parent_handle);

// 把实例当前状态写回模板文件（Apply），返回 0 成功。
GRYCE_CORE_API int GEntity_ApplyPrefab(GEntityHandle entity);

// 还原实例为 模板+覆盖参数 状态（Revert），返回 0 成功。
GRYCE_CORE_API int GEntity_RevertPrefab(GEntityHandle entity);

#ifdef __cplusplus
}
#endif

#endif
