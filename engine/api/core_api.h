#ifndef GRYCE_CORE_API_H
#define GRYCE_CORE_API_H

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

typedef struct {
    uint32_t version;
    const char* project_root;
    bool enable_reflection;
} GCoreInitDesc;

GRYCE_CORE_API int  GCore_Init(const GCoreInitDesc* desc);
GRYCE_CORE_API void GCore_Shutdown(void);
GRYCE_CORE_API bool GCore_IsInitialized(void);

// Game entry: when enabled, GCore_Init loads the project's main scene
// (project.data "main_scene", default res:/scenes/main.gesc) right
// after startup. The editor leaves it disabled. Must be called before
// GCore_Init. Stored via a setter so GCoreInitDesc stays ABI-stable.
GRYCE_CORE_API void GCore_SetAutoLoadMainScene(bool enable);

GRYCE_CORE_API void GCore_BeginFrame(float dt);
GRYCE_CORE_API void GCore_EndFrame(void);

GRYCE_CORE_API int GCore_PushCommand(const GCommand* cmd);
GRYCE_CORE_API int GCore_PushCommands(const GCommand* cmds, int count);
GRYCE_CORE_API int GCore_GetCmdQueueCapacity(void);
GRYCE_CORE_API int GCore_GetDroppedCmdCount(void);

GRYCE_CORE_API bool GCore_IsPlaying(void);
GRYCE_CORE_API bool GCore_IsPaused(void);

GRYCE_CORE_API void GCore_SetCallback_UserData(void* user_data);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnMouseLock(GOnMouseLock cb);

GRYCE_CORE_API int GCore_GetLogMessages(char* out_buf, int buf_size);

// ---------------------------------------------------------------------------
// 撤销 / 重做（快照式）
//
// 每条变更类命令（创建/删除/改名/改父/改变换/改属性/加删组件/改材质/导入）
// 在执行前抓一份场景 JSON 快照入栈；撤销就是把上一份快照反序列化回来。
// 之所以用整场快照而不是增量 diff：场景序列化本来就要写盘、正确性已被存档
// 路径覆盖，且恢复后实体句柄按 UUID 保留（见 EntityHandleMap::rebuild），
// 编辑器持有的选择与面板状态不需要重建。
//
//   * Play 模式下撤销栈冻结（播放期的物理/动画改动本来就不入编辑历史）。
//   * 拖拽类连续编辑用 BeginEdit/EndEdit 包起来，整段只算一步。
//   * 栈深默认 64 步，可用 SetUndoLimit 调整（0 表示禁用记录）。
// ---------------------------------------------------------------------------
GRYCE_CORE_API int  GCore_Undo(void);              // 1 = 执行了撤销，0 = 无可撤销
GRYCE_CORE_API int  GCore_Redo(void);              // 1 = 执行了重做，0 = 无可重做
GRYCE_CORE_API int  GCore_GetUndoCount(void);
GRYCE_CORE_API int  GCore_GetRedoCount(void);
GRYCE_CORE_API void GCore_ClearUndoHistory(void);
GRYCE_CORE_API void GCore_SetUndoLimit(int limit);
// 连续编辑分组：BeginEdit 抓快照，EndEdit 提交为一步（期间的单条命令不再入栈）。
GRYCE_CORE_API void GCore_BeginEdit(const char* label);
GRYCE_CORE_API void GCore_EndEdit(void);

// 渲染侧取走编辑器的离屏目标尺寸请求（core 不能反向调用 GryceRenderer 的
// GViewport_SetSize，只能由渲染侧在帧起点轮询）。target: 0 = 场景，1 = 游戏。
// 返回 1 表示取到一组新尺寸（取走后即清除）。
GRYCE_CORE_API int GCore_TakeRenderTargetRequest(int target, int* out_w, int* out_h);

// ---------------------------------------------------------------------------
// GPack resource packaging (used by GryceGC; format shared with GPackReader).
// ---------------------------------------------------------------------------
typedef void* GPackHandle;

// Create an empty resource pack writer. Returns NULL on failure.
GRYCE_CORE_API GPackHandle GCore_PackCreate(void);

// Add one file to the pack. internal_path uses forward slashes and is relative
// to the project root (e.g. "scenes/main.gesc"). Returns 0 on success.
GRYCE_CORE_API int GCore_PackAddFile(GPackHandle handle, const char* internal_path, const char* source_path);

// Write the pack to output_path (must end in .gpkg/.gpack). Returns 0 on success.
GRYCE_CORE_API int GCore_PackWrite(GPackHandle handle, const char* output_path);

// Free the writer; safe to pass NULL.
GRYCE_CORE_API void GCore_PackDestroy(GPackHandle handle);

// 内部访问：供同进程其他 DLL 模块获取 World 指针（不透明，模块内部再 cast）
GRYCE_CORE_API void* GCore_GetInternalWorldPtr(void);

#ifdef __cplusplus
}
#endif

#endif
