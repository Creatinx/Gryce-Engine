#ifndef GRYCE_TYPES_H
#define GRYCE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int   GEntityHandle;
typedef int   GComponentHandle;
typedef int   GAssetHandle;
typedef void* GTextureHandle;
typedef void* GWindowHandle;
typedef int   GBodyHandle;

typedef struct { float x, y, z; }     GVec3;
typedef struct { float x, y, z, w; }  GVec4;
typedef struct { float x, y, z, w; }  GQuat;
typedef struct { float m[16]; }       GMat4;
typedef struct { float r, g, b, a; }  GColor;

typedef enum {
    GRYCE_RENDER_API_OPENGL = 0,
    GRYCE_RENDER_API_VULKAN = 1,
    GRYCE_RENDER_API_DX11   = 2,
    GRYCE_RENDER_API_DX12   = 3,
} GRenderAPI;

typedef enum {
    GWINDOW_MODE_WINDOWED = 0,
    GWINDOW_MODE_FULLSCREEN,
    GWINDOW_MODE_BORDERLESS,
} GWindowMode;

typedef enum {
    GINPUT_ACTION_PRESS = 0,
    GINPUT_ACTION_RELEASE,
    GINPUT_ACTION_REPEAT,
} GInputAction;

typedef enum {
    GPHYSICS_BACKEND_JOLT = 0,
    GPHYSICS_BACKEND_BOX2D,
} GPhysicsBackend;

typedef void (*GOnEntitySelected)(GEntityHandle entity, void* user_data);
typedef void (*GOnEntityDeselected)(void* user_data);
typedef void (*GOnSceneLoaded)(const char* path, void* user_data);
typedef void (*GOnPlayModeChanged)(bool is_playing, bool is_paused, void* user_data);
typedef void (*GOnEntityListChanged)(void* user_data);
typedef void (*GOnComponentChanged)(GEntityHandle entity, uint64_t comp_type_hash, void* user_data);
typedef void (*GOnLogMessage)(int level, const char* msg, const char* source_file, int source_line, void* user_data);
typedef void (*GOnMouseLock)(int locked, void* user_data);
typedef void (*GOnViewportTextureReady)(GTextureHandle handle, int w, int h, void* user_data);

typedef enum {
    ECMD_NOP = 0,
    ECMD_LOAD_SCENE,
    ECMD_SAVE_SCENE,
    ECMD_CREATE_ENTITY,
    ECMD_DESTROY_ENTITY,
    ECMD_RENAME_ENTITY,
    ECMD_REPARENT_ENTITY,
    ECMD_SELECT_ENTITY,
    ECMD_SET_TRANSFORM,
    ECMD_SET_PROPERTY,
    ECMD_ADD_COMPONENT,
    ECMD_REMOVE_COMPONENT,
    ECMD_PLAY_MODE,
    ECMD_STOP_MODE,
    ECMD_PAUSE_MODE,
    ECMD_STEP_FRAME,
    ECMD_IMPORT_ASSET,

    ECMD_SET_RENDER_TARGET = 100,
    ECMD_SET_VIEWPORT_SIZE,
    ECMD_SET_GAMEVIEW_SIZE,
    ECMD_SET_MATERIAL,

    ECMD_INPUT_KEY = 200,
    ECMD_INPUT_MOUSE_MOVE,
    ECMD_INPUT_MOUSE_BUTTON,
    ECMD_INPUT_MOUSE_SCROLL,
    ECMD_INPUT_MOUSE_RESET,

    ECMD_PHYSICS_SET_GRAVITY = 300,
    ECMD_PHYSICS_ADD_FORCE,

    ECMD_GIZMO_SET_OPERATION = 400,
    ECMD_GIZMO_SET_SPACE,
    ECMD_GIZMO_MANIPULATE,

    // GryceSRT 脚本（Phase 1/2）
    ECMD_SET_SCRIPT = 500,
    ECMD_RELOAD_SCRIPTS,

    ECMD_COUNT
} GCommandType;

#define GCMD_PAYLOAD_SIZE 256

typedef struct {
    GCommandType type;
    uint64_t     seq;
    uint8_t      payload[GCMD_PAYLOAD_SIZE];
} GCommand;

#ifdef __cplusplus
}
#endif

#endif
