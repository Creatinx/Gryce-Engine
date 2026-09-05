#ifndef GRYCE_SCRIPT_API_H
#define GRYCE_SCRIPT_API_H

#include "types.h"

#ifdef _WIN32
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_CORE_API __declspec(dllexport)
    #else
        #define GRYCE_CORE_API __declspec(dllimport)
    #endif
#else
    #define GRYCE_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// GryceSRT C API (smoke-test / editor entry points).
/// Returns the runtime banner, e.g. "GryceSRT 0.2.0 (QuickJS)".
GRYCE_CORE_API const char* GScript_GetVersion(void);

/// Deprecated: use ECS ScriptComponent with QuickJS.
/// Returns -1.
GRYCE_CORE_API int GScript_RunString(const char* code, char* err_out, int err_cap);

/// Deprecated: use ECS ScriptComponent with QuickJS.
/// Returns -1.
GRYCE_CORE_API int GScript_RunFile(const char* path, char* err_out, int err_cap);

/// Exposed script properties (from the script's `props` table).
/// type: 0 = float, 1 = string.
GRYCE_CORE_API int GScript_GetPropCount(GEntityHandle entity, int* out_count);
GRYCE_CORE_API int GScript_GetPropInfo(GEntityHandle entity, int index,
                                       char* name_out, int name_cap, int* out_type);
GRYCE_CORE_API int GScript_GetPropFloat(GEntityHandle entity, const char* name, float* out_value);
GRYCE_CORE_API int GScript_SetPropFloat(GEntityHandle entity, const char* name, float value);
GRYCE_CORE_API int GScript_GetPropString(GEntityHandle entity, const char* name,
                                         char* out_value, int value_cap);
GRYCE_CORE_API int GScript_SetPropString(GEntityHandle entity, const char* name, const char* value);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_SCRIPT_API_H
