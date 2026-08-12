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
// (project_settings.json "main_scene", default res:/scenes/main.gesc) right
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

GRYCE_CORE_API int GCore_GetLogMessages(char* out_buf, int buf_size);

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
