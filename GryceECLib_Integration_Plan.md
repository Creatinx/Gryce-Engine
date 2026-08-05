# Gryce Engine — 模块化 Core / Editor 分离架构方案

> 目标：将现有 `core/` 按命名空间拆分为多个独立 DLL，每个模块对外暴露纯 C API。Editor（WPF C#）**只能通过模块 C API 与 Core 交互**，不再直接 include 任何内部头文件。
>
> 通信模型：**Editor 每帧通过 C API push 命令到各模块的 CommandBuffer，模块在 `ExecuteFrame` 中消费并执行**。
>
> 所有代码统一使用 **C++23**。现有 C++ ImGui Editor 已封存至 `backup/editor/`。

---

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Editor (WPF C#)                                                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  ┌────────┐  │
│  │ Inspector   │  │ Hierarchy   │  │ Viewport / GameView │  │ Console│  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  └───┬────┘  │
│         │                │                    │                 │       │
│         └────────────────┼────────────────────┘                 │       │
│                          ▼                                      │       │
│         ┌─────────────────────────────────────┐                 │       │
│         │  Editor 每帧构造 Command 写入 Buffer │                 │       │
│         │  → Core_PushCommand / Render_PushCmd │                 │       │
│         └──────────────────┬──────────────────┘                 │       │
│                            │ P/Invoke                            │       │
└────────────────────────────┼─────────────────────────────────────┘       │
                             ▼                                             │
┌─────────────────────────────────────────────────────────────────────────┐
│  Module DLLs（按命名空间拆分）                                            │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────┐  │
│  │ GryceCore.dll   │  │GryceRenderer.dll│  │  GrycePlatform.dll      │  │
│  │ ─────────────── │  │ ─────────────── │  │  ─────────────────────  │  │
│  │ ECS / Scene     │  │ RenderContext   │  │  Window / Input / Cursor│  │
│  │ Entity / Comp   │  │ Pipeline / GL   │  │  Timer                  │  │
│  │ Math / UUID     │  │ Vulkan Backend  │  │                         │  │
│  │ Reflection      │  │ Mesh / Texture  │  │                         │  │
│  │ AssetManager    │  │ Shader / Mat    │  │                         │  │
│  │ CommandBuffer   │  │ ImGui (Gizmo)   │  │                         │  │
│  └────────┬────────┘  └────────┬────────┘  └────────────┬────────────┘  │
│           │                    │                        │               │
│  ┌─────────────────┐  ┌─────────────────┐               │               │
│  │ GrycePhysics.dll│  │ GryceAudio.dll  │（optional）   │               │
│  │ ─────────────── │  │ ─────────────── │               │               │
│  │ JoltPhysics 3D  │  │ miniaudio       │               │               │
│  │ Box2D 2D        │  │ AudioEngine     │               │               │
│  └─────────────────┘  └─────────────────┘               │               │
│                                                         │               │
│  模块间依赖（内部 C++ 接口，不对外暴露）                   │               │
│  GryceRenderer → GrycePlatform（Window handle）          │               │
│  GryceCore     → GryceRenderer（RenderSystem 消费）      │               │
│  GryceCore     → GrycePhysics（PhysicsSystem 消费）      │               │
│  GryceCore     → GryceAudio（AudioSystem 消费）          │               │
└─────────────────────────────────────────────────────────────────────────┘
```

### 模块边界

| DLL | 命名空间 | 职责 | C API 头文件 |
|-----|---------|------|------------|
| **GryceCore.dll** | `gryce::core` | ECS World、Scene、Entity、Component、Math、UUID、Reflection、AssetManager、CommandBuffer | `GryceCore/core_api.h` `GryceCore/entity_api.h` `GryceCore/component_api.h` `GryceCore/scene_api.h` |
| **GryceRenderer.dll** | `gryce::render` | RenderContext、RenderPipeline、GL/Vulkan Backend、Mesh、Texture、Shader、Material、Framebuffer、Render2D | `GryceRenderer/render_api.h` `GryceRenderer/viewport_api.h` |
| **GrycePlatform.dll** | `gryce::platform` | Window、InputManager、Cursor、Timer | `GrycePlatform/window_api.h` `GrycePlatform/input_api.h` |
| **GrycePhysics.dll** | `gryce::physics` | PhysicsWorld2D/3D、JoltPhysicsWorld3D、Box2DWorld2D | `GrycePhysics/physics_api.h` |
| **GryceAudio.dll** | `gryce::audio` | AudioEngine、miniaudio | `GryceAudio/audio_api.h`（optional） |

> **Editor 永远不能 `#include "ecs/world.h"` 或 `#include "scene/entity.h"`。所有交互走模块 C API。**

---

## 2. C API 头文件组织

每个模块一组头文件，统一放在模块目录下。共享基础类型放在 `GryceCore/types.h`。

```
core/
├── GryceCore/
│   ├── types.h              ← 共享基础类型（EntityHandle、Vec3、回调签名等）
│   ├── core_api.h           ← World 生命周期、CommandBuffer、PlayMode
│   ├── entity_api.h         ← Entity CRUD、Hierarchy 查询
│   ├── component_api.h      ← Component 增删、属性读写（反射驱动）
│   ├── scene_api.h          ← Scene 加载/保存、序列化
│   └── asset_api.h          ← AssetManager、资源导入
├── GryceRenderer/
│   ├── render_api.h         ← RenderContext 生命周期、Backend 切换
│   ├── viewport_api.h       ← Viewport/GameView 纹理句柄、尺寸
│   └── mesh_api.h           ← Mesh/Texture/Shader 创建（预留）
├── GrycePlatform/
│   ├── window_api.h         ← Window 创建（外部 HWND / GLFW）
│   └── input_api.h          ← Input 状态查询、事件注入
├── GrycePhysics/
│   └── physics_api.h        ← PhysicsWorld 创建、Body 操作、Raycast
└── GryceAudio/
    └── audio_api.h          ← AudioEngine、Source 操作（optional）
```

### 2.1 共享类型：`GryceCore/types.h`

```c
// GryceCore/types.h — 所有模块共享的基础类型
// 此头文件无 DLL 导入/导出，纯类型定义

#ifndef GRYCE_TYPES_H
#define GRYCE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Handle 类型（opaque）
// ---------------------------------------------------------------------------
typedef int   GEntityHandle;      // 0 = invalid
typedef int   GComponentHandle;   // 0 = invalid
typedef int   GAssetHandle;       // 0 = invalid
typedef void* GTextureHandle;     // 平台相关渲染纹理
typedef void* GWindowHandle;      // HWND on Windows
typedef int   GBodyHandle;        // Physics body handle

// ---------------------------------------------------------------------------
// 基础数学类型
// ---------------------------------------------------------------------------
typedef struct { float x, y, z; }     GVec3;
typedef struct { float x, y, z, w; }  GVec4;
typedef struct { float x, y, z, w; }  GQuat;
typedef struct { float m[16]; }       GMat4;
typedef struct { float r, g, b, a; }  GColor;

// ---------------------------------------------------------------------------
// 渲染 API 枚举
// ---------------------------------------------------------------------------
typedef enum {
    GRYCE_RENDER_API_OPENGL = 0,
    GRYCE_RENDER_API_VULKAN = 1,
    GRYCE_RENDER_API_DX11   = 2,   // reserved
    GRYCE_RENDER_API_DX12   = 3,   // reserved
} GRenderAPI;

// ---------------------------------------------------------------------------
// 回调签名（由模块各自注册）
// ---------------------------------------------------------------------------
typedef void (*GOnEntitySelected)(GEntityHandle entity, void* user_data);
typedef void (*GOnEntityDeselected)(void* user_data);
typedef void (*GOnSceneLoaded)(const char* path, void* user_data);
typedef void (*GOnPlayModeChanged)(bool is_playing, bool is_paused, void* user_data);
typedef void (*GOnEntityListChanged)(void* user_data);
typedef void (*GOnComponentChanged)(GEntityHandle entity, uint64_t comp_type_hash, void* user_data);
typedef void (*GOnLogMessage)(int level, const char* msg, void* user_data);
typedef void (*GOnViewportTextureReady)(GTextureHandle handle, int w, int h, void* user_data);

// ---------------------------------------------------------------------------
// 命令基础结构（各模块可扩展 payload）
// ---------------------------------------------------------------------------
typedef enum {
    // Core 命令 (0-99)
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

    // Renderer 命令 (100-199)
    ECMD_SET_RENDER_TARGET = 100,
    ECMD_SET_VIEWPORT_SIZE,
    ECMD_SET_GAMEVIEW_SIZE,
    ECMD_SET_MATERIAL,

    // Platform 命令 (200-299)
    ECMD_INPUT_KEY = 200,
    ECMD_INPUT_MOUSE_MOVE,
    ECMD_INPUT_MOUSE_BUTTON,
    ECMD_INPUT_MOUSE_SCROLL,

    // Physics 命令 (300-399)
    ECMD_PHYSICS_SET_GRAVITY = 300,
    ECMD_PHYSICS_ADD_FORCE,

    // Gizmo 命令 (400-499)
    ECMD_GIZMO_SET_OPERATION = 400,
    ECMD_GIZMO_SET_SPACE,
    ECMD_GIZMO_MANIPULATE,

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

#endif // GRYCE_TYPES_H
```

### 2.2 `GryceCore/core_api.h`

```c
// GryceCore/core_api.h — World 生命周期、CommandBuffer、PlayMode、回调

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

// ========== 初始化 ==========
typedef struct {
    uint32_t version;
    const char* project_root;
    bool enable_reflection;
} GCoreInitDesc;

GRYCE_CORE_API int  GCore_Init(const GCoreInitDesc* desc);
GRYCE_CORE_API void GCore_Shutdown(void);
GRYCE_CORE_API bool GCore_IsInitialized(void);

// ========== 帧驱动 ==========
GRYCE_CORE_API void GCore_BeginFrame(float dt);
GRYCE_CORE_API void GCore_EndFrame(void);

// ========== CommandBuffer（线程安全） ==========
GRYCE_CORE_API int GCore_PushCommand(const GCommand* cmd);
GRYCE_CORE_API int GCore_PushCommands(const GCommand* cmds, int count);
GRYCE_CORE_API int GCore_GetCmdQueueCapacity(void);
GRYCE_CORE_API int GCore_GetDroppedCmdCount(void);

// ========== PlayMode ==========
GRYCE_CORE_API bool GCore_IsPlaying(void);
GRYCE_CORE_API bool GCore_IsPaused(void);

// ========== 回调注册 ==========
GRYCE_CORE_API void GCore_SetCallback_UserData(void* user_data);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb);
GRYCE_CORE_API void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb);

// ========== 日志 ==========
GRYCE_CORE_API int GCore_GetLogMessages(char* out_buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_CORE_API_H
```

### 2.3 `GryceCore/entity_api.h`

```c
// GryceCore/entity_api.h — Entity CRUD、Hierarchy 查询

#ifndef GRYCE_ENTITY_API_H
#define GRYCE_ENTITY_API_H

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

GRYCE_CORE_API int          GEntity_GetCount(void);
GRYCE_CORE_API GEntityHandle GEntity_GetAt(int index);
GRYCE_CORE_API int          GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API int          GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size);
GRYCE_CORE_API GEntityHandle GEntity_GetParent(GEntityHandle entity);
GRYCE_CORE_API int          GEntity_GetChildCount(GEntityHandle entity);
GRYCE_CORE_API GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index);
GRYCE_CORE_API int          GEntity_GetSiblingIndex(GEntityHandle entity);

GRYCE_CORE_API GEntityHandle GEntity_GetSelected(void);

GRYCE_CORE_API int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale);
GRYCE_CORE_API int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos);
GRYCE_CORE_API int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot);
GRYCE_CORE_API int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_ENTITY_API_H
```

### 2.4 `GryceCore/component_api.h`

```c
// GryceCore/component_api.h — Component 增删、属性读写（反射驱动）

#ifndef GRYCE_COMPONENT_API_H
#define GRYCE_COMPONENT_API_H

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

GRYCE_CORE_API int GComponent_GetRegisteredTypeCount(void);
GRYCE_CORE_API int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_COMPONENT_API_H
```

### 2.5 `GryceRenderer/render_api.h`

```c
// GryceRenderer/render_api.h — RenderContext、Backend、帧渲染

#ifndef GRYCE_RENDER_API_H
#define GRYCE_RENDER_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_RENDERER_BUILDING
        #define GRYCE_RENDERER_API __declspec(dllexport)
    #else
        #define GRYCE_RENDERER_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GryceRenderer.lib")
        #endif
    #endif
#else
    #define GRYCE_RENDERER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t     version;
    GWindowHandle native_window;    // HWND on Windows
    GRenderAPI   api;
    int          viewport_w;
    int          viewport_h;
    bool         sync_mode;         // true: 无内部 RenderThread
} GRenderInitDesc;

GRYCE_RENDERER_API int  GRender_Init(const GRenderInitDesc* desc);
GRYCE_RENDERER_API void GRender_Shutdown(void);
GRYCE_RENDERER_API bool GRender_IsInitialized(void);

// 帧渲染（Editor 线程调用）
GRYCE_RENDERER_API void GRender_BeginFrame(void);
GRYCE_RENDERER_API void GRender_RenderWorld(void);   // 调用 ECS RenderSystem
GRYCE_RENDERER_API void GRender_RenderGizmo(void);   // ImGuizmo + Viewport Toolbar
GRYCE_RENDERER_API void GRender_EndFrame(void);

// 视口/GameView 纹理查询
GRYCE_RENDERER_API GTextureHandle GRender_GetViewportTexture(void);
GRYCE_RENDERER_API GTextureHandle GRender_GetGameViewTexture(void);
GRYCE_RENDERER_API int            GRender_GetViewportSize(int* out_w, int* out_h);
GRYCE_RENDERER_API int            GRender_GetGameViewSize(int* out_w, int* out_h);

// VSync
GRYCE_RENDERER_API void GRender_SetVSync(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_RENDER_API_H
```

### 2.6 `GrycePlatform/window_api.h`

```c
// GrycePlatform/window_api.h — Window 抽象（外部 HWND / GLFW 自创建）

#ifndef GRYCE_WINDOW_API_H
#define GRYCE_WINDOW_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_PLATFORM_BUILDING
        #define GRYCE_PLATFORM_API __declspec(dllexport)
    #else
        #define GRYCE_PLATFORM_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GrycePlatform.lib")
        #endif
    #endif
#else
    #define GRYCE_PLATFORM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GWINDOW_MODE_WINDOWED = 0,
    GWINDOW_MODE_FULLSCREEN,
    GWINDOW_MODE_BORDERLESS,
} GWindowMode;

// 外部 HWND 模式：Editor 提供窗口句柄
GRYCE_PLATFORM_API int  GWindow_InitExternal(GWindowHandle hwnd, int w, int h);
// 自创建 GLFW 模式（standalone / examples）
GRYCE_PLATFORM_API int  GWindow_Create(const char* title, int w, int h, GWindowMode mode);
GRYCE_PLATFORM_API void GWindow_Destroy(void);
GRYCE_PLATFORM_API bool GWindow_IsValid(void);

GRYCE_PLATFORM_API void GWindow_GetSize(int* out_w, int* out_h);
GRYCE_PLATFORM_API void GWindow_SetSize(int w, int h);
GRYCE_PLATFORM_API GWindowHandle GWindow_GetNativeHandle(void);
GRYCE_PLATFORM_API bool GWindow_ShouldClose(void);
GRYCE_PLATFORM_API void GWindow_PollEvents(void);
GRYCE_PLATFORM_API void GWindow_SwapBuffers(void);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_WINDOW_API_H
```

### 2.7 `GrycePlatform/input_api.h`

```c
// GrycePlatform/input_api.h — Input 状态查询、事件注入

#ifndef GRYCE_INPUT_API_H
#define GRYCE_INPUT_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_PLATFORM_BUILDING
        #define GRYCE_PLATFORM_API __declspec(dllexport)
    #else
        #define GRYCE_PLATFORM_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GrycePlatform.lib")
        #endif
    #endif
#else
    #define GRYCE_PLATFORM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GINPUT_ACTION_PRESS = 0,
    GINPUT_ACTION_RELEASE,
    GINPUT_ACTION_REPEAT,
} GInputAction;

// Editor 转发输入事件到 Platform 模块
GRYCE_PLATFORM_API void GInput_InjectKey(int key_code, GInputAction action);
GRYCE_PLATFORM_API void GInput_InjectMouseMove(float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseButton(int button, GInputAction action, float x, float y);
GRYCE_PLATFORM_API void GInput_InjectMouseScroll(float delta_x, float delta_y);

// 查询当前输入状态
GRYCE_PLATFORM_API bool GInput_IsKeyPressed(int key_code);
GRYCE_PLATFORM_API bool GInput_IsKeyHeld(int key_code);
GRYCE_PLATFORM_API bool GInput_IsMouseButtonPressed(int button);
GRYCE_PLATFORM_API void GInput_GetMousePosition(float* out_x, float* out_y);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_INPUT_API_H
```

### 2.8 `GrycePhysics/physics_api.h`

```c
// GrycePhysics/physics_api.h — PhysicsWorld 创建、Body 操作、Raycast

#ifndef GRYCE_PHYSICS_API_H
#define GRYCE_PHYSICS_API_H

#include "GryceCore/types.h"

#ifdef _WIN32
    #ifdef GRYCE_PHYSICS_BUILDING
        #define GRYCE_PHYSICS_API __declspec(dllexport)
    #else
        #define GRYCE_PHYSICS_API __declspec(dllimport)
        #ifdef _MSC_VER
            #pragma comment(lib, "GrycePhysics.lib")
        #endif
    #endif
#else
    #define GRYCE_PHYSICS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPHYSICS_BACKEND_JOLT = 0,
    GPHYSICS_BACKEND_BOX2D,
} GPhysicsBackend;

GRYCE_PHYSICS_API int  GPhysics_Init(GPhysicsBackend backend);
GRYCE_PHYSICS_API void GPhysics_Shutdown(void);

GRYCE_PHYSICS_API void GPhysics_SetGravity(const GVec3* gravity);
GRYCE_PHYSICS_API void GPhysics_Step(float dt, int substeps);

GRYCE_PHYSICS_API GBodyHandle GPhysics_CreateBody(GEntityHandle entity, bool is_static);
GRYCE_PHYSICS_API void GPhysics_DestroyBody(GBodyHandle body);
GRYCE_PHYSICS_API void GPhysics_SetBodyTransform(GBodyHandle body, const GVec3* pos, const GQuat* rot);
GRYCE_PHYSICS_API void GPhysics_GetBodyTransform(GBodyHandle body, GVec3* out_pos, GQuat* out_rot);
GRYCE_PHYSICS_API void GPhysics_AddForce(GBodyHandle body, const GVec3* force);
GRYCE_PHYSICS_API void GPhysics_AddImpulse(GBodyHandle body, const GVec3* impulse);

// Raycast: 返回 true 如果命中，out_entity 填命中的 entity handle
GRYCE_PHYSICS_API bool GPhysics_Raycast(const GVec3* origin, const GVec3* dir, float max_dist,
                                         GVec3* out_hit_point, GVec3* out_hit_normal,
                                         GEntityHandle* out_entity);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_PHYSICS_API_H
```

---

## 3. 通信机制：CommandBuffer

### 3.1 设计

CommandBuffer 位于 **GryceCore.dll** 内，是 Editor 与所有模块通信的统一通道。各模块在 `BeginFrame` 时消费属于自己的命令（按 `type` 范围过滤）。

```
Editor Thread
    │
    ▼
┌─────────────────┐     GCore_PushCommand() 写入
│  write_buffer   │  ← 线程安全：CAS spinlock
│  (front buffer) │
└─────────────────┘
    │
    │ GCore_BeginFrame() 内部 swap
    ▼
┌─────────────────┐
│  exec_buffer    │  ← Core 消费
│  (back buffer)  │     按 type 范围分发给各模块
└─────────────────┘
    │
    ▼
Core:     ECMD_LOAD_SCENE / CREATE_ENTITY / SET_TRANSFORM ... → ecs::World / scene::Entity
Renderer: ECMD_SET_RENDER_TARGET / SET_VIEWPORT_SIZE ... → RenderContext
Platform: ECMD_INPUT_KEY / MOUSE_MOVE ... → InputManager
Physics:  ECMD_PHYSICS_SET_GRAVITY ... → IPhysicsWorld
```

- 容量：每缓冲 **8192** 条命令
- 线程安全：Editor 线程 CAS 写入；Core 单线程消费
- 溢出：丢弃旧命令 + log 告警

### 3.2 命令执行时序

```
// Editor 渲染线程（目标 144fps）
loop {
    GCore_BeginFrame(dt);       // swap cmdbuf + 执行 Core 命令 + world.update()
    GRender_BeginFrame();       // backend->begin_frame()
    GRender_RenderWorld();      // ECS RenderSystem
    GRender_RenderGizmo();      // Viewport Toolbar + ImGuizmo
    GRender_EndFrame();         // present
    GCore_EndFrame();           // 触发回调 + 清理
}
```

---

## 4. Core 内部改造要点

### 4.1 CMake 拆分

```cmake
# core/CMakeLists.txt → 拆分为多个 target

# ---------------------------------------------------------------------------
# GryceCore.dll — ECS、Scene、Entity、Component、Math、UUID、Reflection、Assets
# ---------------------------------------------------------------------------
add_library(GryceCore SHARED)
set_target_properties(GryceCore PROPERTIES
    OUTPUT_NAME GryceCore
    RUNTIME_OUTPUT_NAME GryceCore
)
target_compile_definitions(GryceCore PRIVATE GRYCE_CORE_BUILDING)

target_sources(GryceCore PRIVATE
    # C API 实现
    api/core_api.cpp
    api/entity_api.cpp
    api/component_api.cpp
    api/scene_api.cpp
    api/asset_api.cpp
    api/command_buffer.cpp
    api/entity_handle_map.cpp
    # 内部模块
    math/math.cpp
    math/camera.cpp
    utils/glog/glog_lib.cpp
    utils/frame_limiter.cpp
    resources/project.cpp
    resources/resource_path.cpp
    resources/gpack_bundle.cpp
    assets/asset_manager.cpp
    assets/async_loader.cpp
    assets/obj_loader.cpp
    assets/stb_image_impl.cpp
    # ... 其余 core 内部源文件
    scene/uuid.cpp
    scene/entity.cpp
    scene/scene.cpp
    scene/prefab.cpp
    scene/scene_serializer.cpp
    ecs/world.cpp
    ecs/component_store.cpp
    reflection/builtin_reflections.cpp
    components/component_factory.cpp
    # ...
)

# ---------------------------------------------------------------------------
# GryceRenderer.dll — 渲染后端、管线、资源创建
# ---------------------------------------------------------------------------
add_library(GryceRenderer SHARED)
set_target_properties(GryceRenderer PROPERTIES
    OUTPUT_NAME GryceRenderer
    RUNTIME_OUTPUT_NAME GryceRenderer
)
target_compile_definitions(GryceRenderer PRIVATE GRYCE_RENDERER_BUILDING)

target_sources(GryceRenderer PRIVATE
    api/render_api.cpp
    api/viewport_api.cpp
    render/render.cpp
    render/mesh.cpp
    render/shader.cpp
    render/texture.cpp
    render/material.cpp
    render/framebuffer.cpp
    render/render_pipeline.cpp
    render/render_command_buffer.cpp
    render/render_thread.cpp
    render/render_context.cpp
    render/render2d.cpp
    render/font_atlas.cpp
    render/renderer2d_impl.cpp
    render/ibl_generator.cpp
    render/opengl/...
    render/vulkan/...
)
target_link_libraries(GryceRenderer PUBLIC GryceCore)  # 需要 GEntityHandle 等类型

# ---------------------------------------------------------------------------
# GrycePlatform.dll — 窗口、输入、光标
# ---------------------------------------------------------------------------
add_library(GrycePlatform SHARED)
set_target_properties(GrycePlatform PROPERTIES OUTPUT_NAME GrycePlatform)
target_compile_definitions(GrycePlatform PRIVATE GRYCE_PLATFORM_BUILDING)

target_sources(GrycePlatform PRIVATE
    api/window_api.cpp
    api/input_api.cpp
    platform/window.cpp
    platform/input.cpp
    platform/cursor.cpp
)

# ---------------------------------------------------------------------------
# GrycePhysics.dll — Jolt / Box2D
# ---------------------------------------------------------------------------
add_library(GrycePhysics SHARED)
set_target_properties(GrycePhysics PROPERTIES OUTPUT_NAME GrycePhysics)
target_compile_definitions(GrycePhysics PRIVATE GRYCE_PHYSICS_BUILDING)

target_sources(GrycePhysics PRIVATE
    api/physics_api.cpp
    physics/physics_factory.cpp
    physics/box2d_world_2d.cpp
    physics/jolt_physics_world_3d.cpp
)
target_link_libraries(GrycePhysics PUBLIC GryceCore Jolt box2d)

# ---------------------------------------------------------------------------
# GryceAudio.dll — miniaudio（optional）
# ---------------------------------------------------------------------------
# add_library(GryceAudio SHARED ...)
```

### 4.2 模块间依赖（内部 C++ 接口）

```
GryceCore        ← 无依赖（基础层）
GrycePlatform    ← 无依赖
GryceRenderer    ← GryceCore（需要 EntityHandle、GVec3 等类型）
                  ← GrycePlatform（需要 Window handle）
GrycePhysics     ← GryceCore（需要 EntityHandle、GVec3）
GryceAudio       ← GryceCore
```

### 4.3 外部 HWND 注入

```cpp
// GrycePlatform/api/window_api.cpp
int GWindow_InitExternal(GWindowHandle hwnd, int w, int h) {
    // 不调用 glfwCreateWindow
    // 保存 HWND，提供与 GLFW 窗口相同的接口（get_size、poll_events 等）
    // poll_events 在 HWND 模式下为空操作（事件由 Editor 转发）
    // swap_buffers 在 HWND 模式下为空操作（由 Renderer 管理 present）
    g_platform_state.external_hwnd = hwnd;
    g_platform_state.width = w;
    g_platform_state.height = h;
    g_platform_state.is_external = true;
    return 0;
}

// GryceRenderer/api/render_api.cpp
int GRender_Init(const GRenderInitDesc* desc) {
    if (desc->native_window) {
        // Vulkan: vkCreateWin32SurfaceCreateInfoKHR(hwnd)
        // OpenGL: wglCreateContext(GetDC(hwnd))
        render_ctx->init_with_hwnd(desc->native_window, desc->api);
    }
    // sync_mode: 不启动 RenderThread
    if (desc->sync_mode) {
        render_ctx->disable_render_thread();
    }
    return 0;
}
```

### 4.4 EntityHandle 映射

Core 内部仍然使用 `scene::UUID`，对外暴露 opaque `int`。映射表放在 GryceCore.dll 内部：

```cpp
// GryceCore/api/entity_handle_map.cpp
namespace gryce::core {

class HandleMap {
public:
    GEntityHandle alloc(const scene::UUID& uuid);
    scene::Entity* resolve(GEntityHandle h) const;
    void rebuild(const scene::Scene* scene);
    void clear();
private:
    std::unordered_map<int, scene::UUID> handle_to_uuid_;
    std::unordered_map<scene::UUID, int> uuid_to_handle_;
    std::atomic<int> next_handle_{1};
    mutable std::shared_mutex mutex_;
};

} // namespace gryce::core
```

### 4.5 PlayMode 快照

由 GryceCore.dll 内部实现，利用 `SceneSerializer::clone`：

```cpp
// ECMD_PLAY_MODE:
g_core.scene_snapshot = scene::SceneSerializer::clone(*g_core.world->scene());
g_core.world->set_updates_enabled(true);
g_core.play_mode = true;
if (auto* ps3d = g_core.world->get_system<ecs::PhysicsSystem3D>()) {
    ps3d->rebuild_all_bodies();
}
fire_callback(OnPlayModeChanged, true, false);

// ECMD_STOP_MODE:
if (g_core.scene_snapshot) {
    g_core.world->attach_scene(std::move(g_core.scene_snapshot));
    g_core.world->set_updates_enabled(false);
    g_core.play_mode = false;
    g_core.entity_map.rebuild(g_core.world->scene());
    fire_callback(OnEntityListChanged);
    fire_callback(OnPlayModeChanged, false, false);
}
```

---

## 5. Editor（WPF C#）侧设计

### 5.1 P/Invoke 封装（按模块组织）

```csharp
// C# 侧按模块分文件，与 C 头文件一一对应

// GryceCore/Types.cs      → types.h
// GryceCore/CoreAPI.cs    → core_api.h
// GryceCore/EntityAPI.cs  → entity_api.h
// GryceCore/ComponentAPI.cs → component_api.h
// GryceRenderer/RenderAPI.cs → render_api.h
// GrycePlatform/WindowAPI.cs → window_api.h
// GrycePlatform/InputAPI.cs  → input_api.h
// GrycePhysics/PhysicsAPI.cs → physics_api.h

public static class GryceCoreAPI
{
    private const string DllName = "GryceCore.dll";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_Init(ref GCoreInitDesc desc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_BeginFrame(float dt);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_PushCommand(ref GCommand cmd);
}

public static class GryceRendererAPI
{
    private const string DllName = "GryceRenderer.dll";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_Init(ref GRenderInitDesc desc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_BeginFrame();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_RenderWorld();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_EndFrame();
}
```

### 5.2 渲染集成

WPF `SwapChainPanel` / `WindowsFormsHost` 提供 HWND → `GWindow_InitExternal(hwnd, w, h)` → `GRender_Init(..., native_window=hwnd, sync_mode=true)` → Editor 线程每帧驱动 `GRender_BeginFrame/RenderWorld/RenderGizmo/EndFrame`。

### 5.3 输入转发

WPF 鼠标/键盘事件 → `GInput_InjectKey/MouseMove/MouseButton` → GrycePlatform.dll 内部更新 `InputManager` → ECS Systems 消费。

---

## 6. 目录结构

```
Gryce-Engine/
├── CMakeLists.txt
│
├── core/                          # 所有模块源码（按 DLL 组织）
│   ├── CMakeLists.txt             # 定义 GryceCore / GryceRenderer / GrycePlatform / GrycePhysics target
│   │
│   ├── GryceCore/                 # ← C API 头文件（对外发布）
│   │   ├── types.h
│   │   ├── core_api.h
│   │   ├── entity_api.h
│   │   ├── component_api.h
│   │   ├── scene_api.h
│   │   └── asset_api.h
│   │
│   ├── GryceRenderer/
│   │   ├── render_api.h
│   │   ├── viewport_api.h
│   │   └── mesh_api.h
│   │
│   ├── GrycePlatform/
│   │   ├── window_api.h
│   │   └── input_api.h
│   │
│   ├── GrycePhysics/
│   │   └── physics_api.h
│   │
│   ├── api/                       # C API 实现（.cpp）
│   │   ├── core_api.cpp
│   │   ├── entity_api.cpp
│   │   ├── component_api.cpp
│   │   ├── scene_api.cpp
│   │   ├── asset_api.cpp
│   │   ├── command_buffer.cpp
│   │   ├── entity_handle_map.cpp
│   │   ├── render_api.cpp
│   │   ├── viewport_api.cpp
│   │   ├── window_api.cpp
│   │   ├── input_api.cpp
│   │   └── physics_api.cpp
│   │
│   ├── ecs/                       # 现有 ECS（不变）
│   ├── scene/                     # 现有 Scene（不变）
│   ├── render/                    # 现有 Render（+ init_with_hwnd）
│   ├── physics/                   # 现有 Physics（不变）
│   ├── platform/                  # 现有 Platform（+ HostWindow 外部 HWND）
│   ├── assets/                    # 现有 Assets（不变）
│   ├── math/                      # 现有 Math（不变）
│   └── ...
│
├── backup/
│   └── editor/                    # ← 封存的原 C++ ImGui Editor
│
├── editor_wpf/                    # （未来）C# WPF Editor
│   └── ...
│
├── examples/                      # Standalone 示例
│   └── ...
│
└── tests/                         # 单元测试
```

---

## 7. 实施步骤

### Phase 0: 骨架（先完成）
- [x] Git checkpoint: `b099c19`
- [x] 现有 ImGui Editor 封存至 `backup/editor/`
- [ ] 创建目录结构：`core/GryceCore/`、`core/GryceRenderer/`、`core/GrycePlatform/`、`core/GrycePhysics/`
- [ ] 创建所有 C API 头文件（空骨架，编译通过）
- [ ] 修改 `core/CMakeLists.txt`：拆分 `add_library(gryce_core STATIC)` → 4 个 `add_library(... SHARED)`
- [ ] 创建 `core/api/` 目录，放置空 C API 实现 `.cpp`
- [ ] 编译通过，生成 `GryceCore.dll` + `GryceRenderer.dll` + `GrycePlatform.dll` + `GrycePhysics.dll`

### Phase 1: GryceCore.dll 可运行
1. `GCore_Init` / `GCore_Shutdown` — 内部创建 `ecs::World`
2. `GCore_BeginFrame` / `GCore_EndFrame` — 空 world.update()
3. `CommandBuffer` 双缓冲实现
4. `ECMD_LOAD_SCENE`、`ECMD_CREATE_ENTITY`、`ECMD_SELECT_ENTITY`
5. `EntityHandle` 映射 + `GEntity_GetCount` / `GetName` 等查询
6. 回调系统：`OnSceneLoaded`、`OnEntitySelected`、`OnEntityListChanged`

### Phase 2: GryceRenderer.dll + GrycePlatform.dll
1. `GWindow_InitExternal` — 接收 HWND，不创建 GLFW
2. `GRender_Init` — `init_with_hwnd` + sync_mode
3. `GRender_BeginFrame` / `RenderWorld` / `RenderGizmo` / `EndFrame`
4. Viewport texture 输出：`GRender_GetViewportTexture`
5. Standalone test：Editor 提供 HWND → 看到清屏色 + 空场景

### Phase 3: 场景操作闭环
1. `ECMD_SET_TRANSFORM`、`ECMD_ADD_COMPONENT`、`ECMD_SET_PROPERTY`
2. `GComponent_GetProperty` / `SetProperty`（反射桥接）
3. WPF/C++ test：加载场景 → Hierarchy 显示 → 选中 → Inspector 修改 → Viewport 更新

### Phase 4: PlayMode + 物理 + Gizmo
1. `ECMD_PLAY_MODE` / `ECMD_STOP_MODE` + 场景快照
2. `GrycePhysics.dll`：`GPhysics_Init` / `Step` / `Raycast`
3. `GrycePlatform.dll`：输入事件注入 `GInput_InjectKey/Mouse`
4. Core 内部绘制 Viewport Toolbar + ImGuizmo
5. Gizmo 操作通过 `ECMD_GIZMO_MANIPULATE` 写回 Transform

### Phase 5: 资产 + 完整闭环
1. `GryceCore/asset_api.h` + `ECMD_IMPORT_ASSET`
2. 日志转发：`GCore_GetLogMessages`
3. 完整 Editor ↔ Core 闭环验证

---

## 8. 风险与缓解

| 风险 | 说明 | 缓解方案 |
|------|------|---------|
| CMake 拆分后循环依赖 | GryceRenderer 需要 GEntityHandle，GryceCore 的 RenderSystem 需要 Renderer | GryceCore 只保留 `ISystem` 接口和 `RenderSystem` 骨架，具体渲染实现通过回调或虚表注入；或者 GryceCore 链接 GryceRenderer 作为 PRIVATE 依赖 |
| RenderContext RenderThread 冲突 | WPF 要求独立线程调用渲染 | SyncMode：GryceRenderer 不启动内部 RenderThread |
| 外部 HWND + Vulkan | `vkCreateWin32SurfaceKHR` + HWND | Phase 2 先验证 OpenGL（wgl 更简单），Vulkan 跟进 |
| `dynamic_cast` 与 `-fno-rtti` | `World::get_system<T>()` 用了 `dynamic_cast` | 替换为 typeid hash + `static_cast`，或编译期索引表 |
| C# struct 内存对齐 | payload 固定 256 字节 | C# 侧 `StructLayout(LayoutKind.Sequential, Size = 264)` |
| DLL 数量多导致部署复杂 | 4-5 个 DLL + 依赖项 | 提供一键打包脚本；Phase 0 先保证编译通过 |

---

## 9. 立即行动

确认本方案后，立即开始 **Phase 0**：
1. 创建 `core/GryceCore/`、`core/GryceRenderer/`、`core/GrycePlatform/`、`core/GrycePhysics/` 头文件目录
2. 创建 `core/api/` 实现目录
3. 修改 `core/CMakeLists.txt` 拆分为 4 个 SHARED target
4. 空实现编译通过，生成 4 个 DLL
