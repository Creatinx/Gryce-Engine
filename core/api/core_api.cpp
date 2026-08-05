#include "GryceCore/core_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/component_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/asset_api.h"

// Placeholder implementations — will be filled in Phase 1

extern "C" {

int GCore_Init(const GCoreInitDesc* desc) { (void)desc; return 0; }
void GCore_Shutdown(void) {}
bool GCore_IsInitialized(void) { return false; }

void GCore_BeginFrame(float dt) { (void)dt; }
void GCore_EndFrame(void) {}

int GCore_PushCommand(const GCommand* cmd) { (void)cmd; return -1; }
int GCore_PushCommands(const GCommand* cmds, int count) { (void)cmds; (void)count; return -1; }
int GCore_GetCmdQueueCapacity(void) { return 0; }
int GCore_GetDroppedCmdCount(void) { return 0; }

bool GCore_IsPlaying(void) { return false; }
bool GCore_IsPaused(void) { return false; }

void GCore_SetCallback_UserData(void* user_data) { (void)user_data; }
void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb) { (void)cb; }
void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb) { (void)cb; }
void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb) { (void)cb; }
void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb) { (void)cb; }
void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb) { (void)cb; }
void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb) { (void)cb; }
void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb) { (void)cb; }

int GCore_GetLogMessages(char* out_buf, int buf_size) { (void)out_buf; (void)buf_size; return 0; }

} // extern "C"
