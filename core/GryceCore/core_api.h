#ifndef GRYCE_CORE_API_H
#define GRYCE_CORE_API_H

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

typedef struct {
    uint32_t version;
    const char* project_root;
    bool enable_reflection;
} GCoreInitDesc;

GRYCE_CORE_API int  GCore_Init(const GCoreInitDesc* desc);
GRYCE_CORE_API void GCore_Shutdown(void);
GRYCE_CORE_API bool GCore_IsInitialized(void);

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

// 内部访问：供同进程其他 DLL 模块获取 World 指针（不透明，模块内部再 cast）
GRYCE_CORE_API void* GCore_GetInternalWorldPtr(void);

#ifdef __cplusplus
}
#endif

#endif
