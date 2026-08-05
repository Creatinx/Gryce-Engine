# Gryce Engine — Core / Editor 分离架构方案

> 目标：把 `core/` 编译为 **GryceCore.dll**（SHARED），对外暴露纯 C API。Editor（WPF C#）**只能通过 C API 与 Core 交互**。所有代码统一使用 **C++23**。
>
> 通信模型：**Editor 每帧通过 C API push 命令到 Core 的 CommandBuffer，Core 在下一帧 `ExecuteFrame` 中消费并执行**。
>
> 现有 C++ ImGui Editor 已封存至 `backup/editor/`，不再维护。

---

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│  Editor (WPF C#)                                            │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ Inspector   │  │ Hierarchy   │  │ Viewport / GameView │  │
│  │ (PropertyGrid│  │ (TreeView)  │  │ (SwapChainPanel)    │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
│         │                │                    │              │
│         └────────────────┼────────────────────┘              │
│                          ▼                                   │
│         ┌─────────────────────────────────────┐              │
│         │  Editor 每帧构造 Command 写入 Buffer │              │
│         │  （SelectEntity / SetTransform /    │              │
│         │   LoadScene / PlayMode / etc.）      │              │
│         └──────────────────┬──────────────────┘              │
│                            │ P/Invoke                        │
└────────────────────────────┼─────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│  GryceCore.dll  ← 由 core/ 编译的 SHARED DLL               │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  C API Layer (gryce_core.h)  ← 唯一对外头文件          │  │
│  │  ─────────────────────────────────────────────────────│  │
│  │  Core_Init() / Core_Shutdown()                        │  │
│  │  Core_BeginFrame() / Core_Render() / Core_EndFrame()  │  │
│  │  Core_PushCommand()                                   │  │
│  │  Core_RegisterCallback()                              │  │
│  │  Core_GetEntityCount() / Core_GetEntityName()  etc.   │  │
│  └───────────────────────────────────────────────────────┘  │
│                            │                                 │
│         ┌──────────────────┼──────────────────┐              │
│         ▼                  ▼                  ▼              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ ECS / Scene │  │  Rendering  │  │ Physics (Jolt/Box2D)│  │
│  │ (ecs::World)│  │(RenderContext│  │ (IPhysicsWorld)    │  │
│  │(ComponentStore│ │ + Pipeline) │  │                     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  Platform   │  │   Assets    │  │      Input          │  │
│  │ (Window/GLFW)│  │(AssetManager)│  │  (InputManager)    │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  CommandBuffer — 双缓冲 Lock-free Ring Buffer          │  │
│  │  Editor 写 front，Core 在 BeginFrame 时 swap & 消费    │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 关键边界

| Core 内部（GryceCore.dll） | Editor 外部（任何语言） |
|---------------------------|----------------------|
| `ecs::World`, `scene::Scene`, `scene::Entity` | 只能看到 `EntityHandle`（int id） |
| `render::RenderContext`, `render::RenderPipeline` | 只能调用 `Core_Render()` |
| `components::MeshRenderer`, `Transform`, `Camera` | 只能通过 `Core_SetComponentProperty()` 修改 |
| `assets::AssetManager` | 只能通过 `Core_LoadAsset()` / `Core_ImportAsset()` |
| `physics::IPhysicsWorld` | 只能通过 PlayMode 触发 step |
| 输入从 GLFW 轮询（standalone） | 输入由 Editor 转发事件到 `Core_PushInputEvent()` |

> **Editor 永远不能 `#include "ecs/world.h"` 或 `#include "scene/entity.h"`。所有交互走 `gryce_core.h` 这一个头文件。**

---

## 2. 单头文件 C API：`gryce_core.h`

使用者（C/C++/C#）**只需 `#include <gryce_core.h>`**，无需手动链接。头文件内部自动处理平台相关的导入库链接。

```c
// gryce_core.h — Editor 唯一允许 include 的头文件
// 编译时自动链接 GryceCore.lib（MSVC）或等效机制

#ifndef GRYCE_CORE_H
#define GRYCE_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 平台自动链接导入库
// ============================================================================
#ifdef _WIN32
    #ifdef _MSC_VER
        // MSVC: #pragma comment 自动链接导入库
        #ifdef GRYCE_CORE_STATIC
            // 静态链接模式（预留）
        #else
            #pragma comment(lib, "GryceCore.lib")
        #endif
    #endif
    // DLL 导出/导入声明
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_API __declspec(dllexport)
    #else
        #define GRYCE_API __declspec(dllimport)
    #endif
#else
    #define GRYCE_API __attribute__((visibility("default")))
#endif

// ============================================================================
// 基础类型
// ============================================================================
typedef int   EntityHandle;     // 0 = invalid/null
typedef int   ComponentHandle;  // 0 = invalid
typedef int   AssetHandle;      // 0 = invalid
typedef void* TextureHandle;    // 平台相关渲染纹理句柄

// ============================================================================
// 初始化描述
// ============================================================================
typedef enum {
    GRYCE_RENDER_API_OPENGL = 0,
    GRYCE_RENDER_API_VULKAN = 1,
} GryceRenderAPI;

typedef struct {
    uint32_t version;           // sizeof(CoreInitDesc)，用于未来扩展
    void*    native_window;     // HWND on Windows
    GryceRenderAPI render_api;
    const char* project_root;   // 资源根目录（绝对路径）
    int      viewport_w;
    int      viewport_h;
    int      gameview_w;        // 0 = 不创建 GameView
    int      gameview_h;
    bool     sync_render_mode;  // true: 不启动内部 RenderThread，由 Editor 线程驱动
} CoreInitDesc;

// ============================================================================
// 命令系统
// ============================================================================
typedef enum {
    ECMD_NOP = 0,

    // --- 场景操作 ---
    ECMD_LOAD_SCENE,            // payload: const char* path
    ECMD_SAVE_SCENE,            // payload: const char* path
    ECMD_CREATE_ENTITY,         // payload: const char* name, parent_handle
    ECMD_DESTROY_ENTITY,        // payload: EntityHandle
    ECMD_RENAME_ENTITY,         // payload: EntityHandle, const char* new_name
    ECMD_REPARENT_ENTITY,       // payload: EntityHandle, new_parent_handle

    // --- 实体选择 / Inspector ---
    ECMD_SELECT_ENTITY,         // payload: EntityHandle (0 = deselect)
    ECMD_SET_TRANSFORM,         // payload: EntityHandle, pos[3], rot[4], scale[3]
    ECMD_SET_PROPERTY,          // payload: EntityHandle, comp_type_hash, field_hash, value_bytes[]
    ECMD_ADD_COMPONENT,         // payload: EntityHandle, comp_type_hash
    ECMD_REMOVE_COMPONENT,      // payload: EntityHandle, comp_type_hash

    // --- Play Mode ---
    ECMD_PLAY_MODE,             // 无 payload
    ECMD_STOP_MODE,             // 无 payload
    ECMD_PAUSE_MODE,            // 无 payload
    ECMD_STEP_FRAME,            // payload: int step_count

    // --- 资产 ---
    ECMD_IMPORT_ASSET,          // payload: const char* source_path
    ECMD_SET_MATERIAL,          // payload: EntityHandle, material_path

    // --- 渲染 ---
    ECMD_SET_RENDER_TARGET,     // payload: HWND handle (0 = offscreen)
    ECMD_SET_VIEWPORT_SIZE,     // payload: w, h
    ECMD_SET_GAMEVIEW_SIZE,     // payload: w, h

    // --- 输入（由 Editor 转发） ---
    ECMD_INPUT_KEY,             // payload: key_code, action(press/release/repeat)
    ECMD_INPUT_MOUSE_MOVE,      // payload: x, y
    ECMD_INPUT_MOUSE_BUTTON,    // payload: button, action, x, y
    ECMD_INPUT_MOUSE_SCROLL,    // payload: delta_x, delta_y

    // --- Gizmo ---
    ECMD_GIZMO_SET_OPERATION,   // payload: int op (translate/rotate/scale)
    ECMD_GIZMO_SET_SPACE,       // payload: int space (local/world)
    ECMD_GIZMO_MANIPULATE,      // payload: EntityHandle, delta_matrix[16]

    ECMD_COUNT                  // 哨兵
} ECommandType;

#define ECMD_PAYLOAD_SIZE 256

typedef struct {
    ECommandType type;
    uint64_t     seq;                   // monotonic sequence
    uint8_t      payload[ECMD_PAYLOAD_SIZE];
} ECommand;

// ============================================================================
// 回调类型（Core → Editor）
// ============================================================================
typedef void (*OnEntitySelected)(EntityHandle entity, void* user_data);
typedef void (*OnEntityDeselected)(void* user_data);
typedef void (*OnSceneLoaded)(const char* path, void* user_data);
typedef void (*OnSceneSaved)(const char* path, void* user_data);
typedef void (*OnPlayModeChanged)(bool is_playing, bool is_paused, void* user_data);
typedef void (*OnEntityListChanged)(void* user_data);           // 创建/删除/重命名/重排
ntypedef void (*OnComponentChanged)(EntityHandle entity, uint64_t comp_type_hash, void* user_data);
typedef void (*OnLogMessage)(int level, const char* msg, void* user_data);
typedef void (*OnViewportTextureReady)(TextureHandle handle, int w, int h, void* user_data);
typedef void (*OnGameViewTextureReady)(TextureHandle handle, int w, int h, void* user_data);

// ============================================================================
// C API 函数
// ============================================================================

// --- 生命周期 ---
GRYCE_API int  Core_Init(const CoreInitDesc* desc);
GRYCE_API void Core_Shutdown(void);
GRYCE_API bool Core_IsInitialized(void);

// --- 帧驱动（由 Editor 的渲染线程按固定频率调用，如 144fps） ---
GRYCE_API void Core_BeginFrame(float dt);
GRYCE_API void Core_Render(void);
GRYCE_API void Core_EndFrame(void);

// --- 命令提交（线程安全，可从 Editor UI 线程随时调用） ---
GRYCE_API int Core_PushCommand(const ECommand* cmd);
GRYCE_API int Core_PushCommands(const ECommand* cmds, int count);
GRYCE_API int Core_GetCommandQueueCapacity(void);    // 返回剩余可写入命令数
GRYCE_API int Core_GetDroppedCommandCount(void);      // 自上次调用以来因队列满丢弃的命令数

// --- 回调注册 ---
GRYCE_API void Core_SetCallback_UserData(void* user_data);
GRYCE_API void Core_RegisterCallback_OnEntitySelected(OnEntitySelected cb);
GRYCE_API void Core_RegisterCallback_OnEntityDeselected(OnEntityDeselected cb);
GRYCE_API void Core_RegisterCallback_OnSceneLoaded(OnSceneLoaded cb);
GRYCE_API void Core_RegisterCallback_OnPlayModeChanged(OnPlayModeChanged cb);
GRYCE_API void Core_RegisterCallback_OnEntityListChanged(OnEntityListChanged cb);
GRYCE_API void Core_RegisterCallback_OnComponentChanged(OnComponentChanged cb);
GRYCE_API void Core_RegisterCallback_OnLogMessage(OnLogMessage cb);
GRYCE_API void Core_RegisterCallback_OnViewportTextureReady(OnViewportTextureReady cb);
GRYCE_API void Core_RegisterCallback_OnGameViewTextureReady(OnGameViewTextureReady cb);

// --- 场景查询（只读，Editor 用来刷新 Hierarchy / Inspector） ---
GRYCE_API int          Core_GetEntityCount(void);
GRYCE_API EntityHandle Core_GetEntityAt(int index);
GRYCE_API int          Core_GetEntityName(EntityHandle entity, char* out_buf, int buf_size);
GRYCE_API int          Core_GetEntityPath(EntityHandle entity, char* out_buf, int buf_size);  // "Parent/Child/Name"
GRYCE_API EntityHandle Core_GetEntityParent(EntityHandle entity);       // 0 = root
GRYCE_API int          Core_GetEntityChildCount(EntityHandle entity);
GRYCE_API EntityHandle Core_GetEntityChildAt(EntityHandle entity, int index);
GRYCE_API int          Core_GetEntitySiblingIndex(EntityHandle entity);

GRYCE_API int          Core_GetEntityComponentCount(EntityHandle entity);
GRYCE_API int          Core_GetEntityComponentTypeHashAt(EntityHandle entity, int index, uint64_t* out_hash);
GRYCE_API int          Core_GetEntityComponentTypeNameAt(EntityHandle entity, int index, char* out_buf, int buf_size);

// 获取组件属性（反射驱动）
GRYCE_API int Core_GetComponentPropertyCount(EntityHandle entity, uint64_t comp_type_hash);
GRYCE_API int Core_GetComponentPropertyInfo(EntityHandle entity, uint64_t comp_type_hash, int prop_index,
                                             char* out_name, int name_buf_size,
                                             int* out_type, int* out_size, int* out_offset);
GRYCE_API int Core_GetComponentProperty(EntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                         void* out_value, int value_size);
GRYCE_API int Core_SetComponentProperty(EntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                         const void* value, int value_size);

GRYCE_API EntityHandle Core_GetSelectedEntity(void);   // 0 = none

// --- 变换快捷查询 ---
GRYCE_API int Core_GetEntityLocalPosition(EntityHandle entity, float out_pos[3]);
GRYCE_API int Core_GetEntityLocalRotation(EntityHandle entity, float out_rot[4]);
GRYCE_API int Core_GetEntityLocalScale(EntityHandle entity, float out_scale[3]);
GRYCE_API int Core_GetEntityWorldPosition(EntityHandle entity, float out_pos[3]);
GRYCE_API int Core_GetEntityWorldRotation(EntityHandle entity, float out_rot[4]);
GRYCE_API int Core_GetEntityWorldScale(EntityHandle entity, float out_scale[3]);

// --- 渲染查询 ---
GRYCE_API TextureHandle Core_GetViewportTextureHandle(void);
GRYCE_API TextureHandle Core_GetGameViewTextureHandle(void);
GRYCE_API int           Core_GetViewportTextureSize(int* out_w, int* out_h);
GRYCE_API int           Core_GetGameViewTextureSize(int* out_w, int* out_h);

// --- PlayMode 状态 ---
GRYCE_API bool Core_IsPlaying(void);
GRYCE_API bool Core_IsPaused(void);

// --- 日志 ---
GRYCE_API int Core_GetLogMessages(char* out_buf, int buf_size);   // 消费并返回日志文本

// --- 反射 / 类型信息 ---
GRYCE_API int Core_GetRegisteredComponentTypeCount(void);
GRYCE_API int Core_GetRegisteredComponentTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_CORE_H
```

---

## 3. 通信机制：CommandBuffer

### 3.1 双缓冲设计

```
Editor Thread (WPF UI Thread / 任意线程)
    │
    ▼
┌─────────────────┐     Core_PushCommand() 写入
│  write_buffer   │  ← 当前帧编辑器写命令到这里
│  (front buffer) │     线程安全：单生产者 CAS spinlock
└─────────────────┘
    │
    │ Core_BeginFrame() 内部自动 swap
    ▼
┌─────────────────┐
│  exec_buffer    │  ← Core 消费上一帧的命令
│  (back buffer)  │     Core 单线程访问，无锁
└─────────────────┘
    │
    ▼
Core 内部: 逐个 ECommand → 调用 ecs::World / scene::Entity / RenderContext
```

- 容量：每缓冲 **8192** 条命令（可配置）
- 溢出策略：环形覆盖（旧命令丢弃）+ `OnLogMessage` 通知 Editor "commands dropped"
- 线程安全：Editor 线程通过 CAS 写入 front buffer；Core 在 `BeginFrame` 时原子 swap 两个 `std::atomic<size_t>*` 指针

### 3.2 命令执行时序

```
Core_BeginFrame(dt):
    1. swap_command_buffers()           // 原子交换 front/back 指针
    2. consume_all_commands(exec_buffer) // 按 seq 顺序执行
    3. 如果是 PlayMode: world->update(dt)
    4. 如果是 PlayMode: phys_world->step(dt)

Core_Render():
    1. backend->begin_frame()
    2. world->render(render_ctx)        // ECS RenderSystem
    3. render viewport toolbar + gizmo  // Core 内部 ImGui（仅 Viewport 区域）
    4. backend->end_frame() / present

Core_EndFrame():
    1. 触发回调（entity list changed, selection changed, texture ready 等）
    2. 清理本帧临时资源
```

---

## 4. Core 内部改造要点

### 4.1 `core/` → `GryceCore.dll`

修改 `core/CMakeLists.txt`：

```cmake
# core/CMakeLists.txt
add_library(gryce_core SHARED)

set_target_properties(gryce_core PROPERTIES
    OUTPUT_NAME GryceCore
    RUNTIME_OUTPUT_NAME GryceCore
    EXPORT_NAME GryceCore
)

target_compile_definitions(gryce_core PRIVATE GRYCE_CORE_BUILDING)
```

新增文件（位于 `core/` 根目录）：

| 文件 | 职责 |
|------|------|
| `gryce_core.h` | **唯一对外 C API 头文件**，使用者 `#include` 即可 |
| `gryce_core_api.cpp` | C API 实现，Core 内部全局状态管理 |
| `command_buffer.h/.cpp` | 双缓冲 lock-free 命令队列 |
| `entity_handle_map.h/.cpp` | `EntityHandle(int) ↔ UUID` 映射 + 反射查询桥接 |

### 4.2 全局状态

```cpp
// core/gryce_core_api.cpp
namespace gryce_core {

struct GlobalState {
    std::unique_ptr<ecs::World> world;
    std::unique_ptr<render::RenderContext> render_ctx;
    std::unique_ptr<render::ImGuiRenderer> imgui;      // 仅 Viewport Toolbar + Gizmo
    std::unique_ptr<physics::IPhysicsWorld3D> phys_world;

    // CommandBuffer 双缓冲
    CommandBuffer cmdbuf;

    // EntityHandle ↔ UUID
    HandleMap entity_map;

    // 回调
    CallbackTable callbacks;
    void* callback_user_data = nullptr;

    // 运行时状态
    bool initialized = false;
    bool play_mode = false;
    bool paused = false;
    std::unique_ptr<scene::Scene> scene_snapshot;

    // 窗口
    void* native_window = nullptr;      // HWND
    bool external_window = false;       // true: 不创建 GLFW 窗口
};

static GlobalState g_state;

} // namespace gryce_core
```

### 4.3 外部 HWND 注入

```cpp
// Core_Init 中的窗口初始化分支
if (desc->native_window) {
    // 外部 HWND 模式
    g_state.external_window = true;
    g_state.native_window = desc->native_window;

    // RenderContext::init 新增分支：识别外部 HWND
    // Vulkan: vkCreateWin32SurfaceCreateInfoKHR(hwnd) 替代 glfwCreateWindowSurface
    // OpenGL: wglCreateContext(GetDC(hwnd)) + wglMakeCurrent
    render_ctx->init_with_hwnd(desc->native_window, desc->render_api);
} else {
    // Standalone 模式：保留现有 GLFW 窗口创建逻辑
    auto window = std::make_unique<platform::Window>(...);
    render_ctx->init(window->native_handle(), desc->render_api);
}
```

### 4.4 渲染模式

| 模式 | 说明 | 标志 |
|------|------|------|
| **SyncMode** | 不启动 `RenderThread`，Editor 线程直接调用 `backend->begin_frame/end_frame` | `desc->sync_render_mode = true` |
| **AsyncMode** | 启动 `RenderThread`，Core 内部异步 present | `desc->sync_render_mode = false` |

WPF 场景强制 **SyncMode**：WPF 在独立线程按目标帧率调用 `Core_BeginFrame / Render / EndFrame`，Core 内部不创建额外渲染线程。

Standalone / examples 用 **AsyncMode**：保留现有 `render_ctx.start()` 行为。

### 4.5 EntityHandle 映射

Core 内部仍然使用 `scene::UUID`，对外暴露 opaque `int`：

```cpp
// core/entity_handle_map.cpp
EntityHandle HandleMap::alloc(const scene::UUID& uuid) {
    int h = next_handle.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    handle_to_uuid[h] = uuid;
    uuid_to_handle[uuid] = h;
    return h;
}

scene::Entity* HandleMap::resolve(EntityHandle h) const {
    std::lock_guard lock(mutex);
    auto it = handle_to_uuid.find(h);
    if (it == handle_to_uuid.end()) return nullptr;
    return g_state.world->scene()->find_entity_by_uuid(it->second);
}

// PlayMode 快照/恢复时：handle → uuid 映射保持不变
// 场景 reload 时：重建映射表，原有 handle 失效，Editor 收到 OnSceneLoaded 回调后刷新
```

### 4.6 PlayMode 快照

```cpp
// ECMD_PLAY_MODE:
g_state.scene_snapshot = scene::SceneSerializer::clone(*g_state.world->scene());
g_state.world->set_updates_enabled(true);
g_state.play_mode = true;
g_state.paused = false;
if (auto* ps3d = g_state.world->get_system<ecs::PhysicsSystem3D>()) {
    ps3d->rebuild_all_bodies();  // 确保物理 body 与 scene 同步
}
fire_callback(OnPlayModeChanged, true, false);

// ECMD_STOP_MODE:
if (g_state.scene_snapshot) {
    g_state.world->attach_scene(std::move(g_state.scene_snapshot));
    g_state.world->set_updates_enabled(false);
    g_state.play_mode = false;
    g_state.paused = false;
    if (auto* ps3d = g_state.world->get_system<ecs::PhysicsSystem3D>()) {
        ps3d->rebuild_all_bodies();
    }
    // 重建 handle map（scene 重建了，UUID 可能变化）
    g_state.entity_map.rebuild_from_scene(g_state.world->scene());
    fire_callback(OnEntityListChanged);
    fire_callback(OnPlayModeChanged, false, false);
}
```

---

## 5. Editor（WPF C#）侧设计

### 5.1 P/Invoke 封装

```csharp
// GryceCore.cs — C# 对 gryce_core.h 的封装
using System;
using System.Runtime.InteropServices;

public static class GryceCore
{
    private const string DllName = "GryceCore.dll";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Core_Init(ref CoreInitDesc desc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Core_BeginFrame(float dt);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Core_Render();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Core_EndFrame();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int Core_PushCommand(ref ECommand cmd);

    // 命令快捷方法
    public static void SelectEntity(int entityHandle)
    {
        var cmd = new ECommand { type = ECommandType.ECMD_SELECT_ENTITY };
        unsafe {
            *(int*)cmd.payload = entityHandle;
        }
        Core_PushCommand(ref cmd);
    }

    public static void SetTransform(int entityHandle, Vector3 pos, Quaternion rot, Vector3 scale)
    {
        var cmd = new ECommand { type = ECommandType.ECMD_SET_TRANSFORM };
        // 序列化到 payload...
        Core_PushCommand(ref cmd);
    }

    // 回调注册...
}
```

### 5.2 渲染集成

- **Viewport**: `SwapChainPanel` / `WindowsFormsHost` 承载 Core 渲染输出
- Core 将 Viewport 渲染到 offscreen texture → Editor 通过 `Core_GetViewportTextureHandle()` 获取 texture handle 并绑定到 WPF 的 D3D11 interop
- 或者 Core 直接在 Editor 提供的 HWND 上渲染（OpenGL/Vulkan 共享 context）

### 5.3 输入转发

WPF 鼠标/键盘事件 → `Core_PushCommand(ECMD_INPUT_MOUSE_MOVE / KEY / BUTTON)` → Core 内部更新 `InputManager` 状态 → ECS Systems 消费。

---

## 6. 目录结构

```
Gryce-Engine/
├── CMakeLists.txt
│
├── core/                          # GryceCore.dll 源码
│   ├── CMakeLists.txt             # add_library(gryce_core SHARED, OUTPUT_NAME GryceCore)
│   ├── gryce_core.h               # ← 唯一对外 C API 头文件（自动链接 GryceCore.lib）
│   ├── gryce_core_api.cpp         # C API 实现
│   ├── command_buffer.h/.cpp      # 双缓冲命令队列
│   ├── entity_handle_map.h/.cpp   # Handle ↔ UUID 映射
│   ├── ecs/                       # 现有 ECS（不变）
│   ├── scene/                     # 现有 Scene（不变）
│   ├── render/                    # 现有 Render（+ init_with_hwnd）
│   ├── physics/                   # 现有 Physics（不变）
│   ├── platform/                  # 现有 Platform（+ HostWindow 外部 HWND 支持）
│   ├── assets/                    # 现有 Assets（不变）
│   ├── math/                      # 现有 Math（不变）
│   └── ...
│
├── backup/
│   └── editor/                    # ← 封存的原 C++ ImGui Editor（不再维护）
│       ├── CMakeLists.txt
│       ├── editor_app.cpp
│       └── ...
│
├── editor_wpf/                    # （未来）C# WPF Editor
│   └── ...
│
├── examples/                      # Standalone 示例（走 C API 或直接链接 core）
│   └── ...
│
└── tests/                         # 单元测试
```

---

## 7. 实施步骤

### Phase 0: 骨架（先完成）
- [x] Git checkpoint: `b099c19`
- [x] 现有 ImGui Editor 封存至 `backup/editor/`
- [ ] 新建 `core/gryce_core.h`（单头文件 C API，含自动链接）
- [ ] 修改 `core/CMakeLists.txt`：输出 `GryceCore.dll` + `GryceCore.lib`
- [ ] 新建 `core/gryce_core_api.cpp`（空实现，编译通过）

### Phase 1: 最小可运行 C API
1. `Core_Init` / `Core_Shutdown` — 内部创建 `ecs::World`、`RenderContext`
2. `Core_BeginFrame` / `Core_Render` / `Core_EndFrame` — 清屏 + 空场景
3. 外部 HWND 模式（`init_with_hwnd`）
4. SyncMode（不启动 RenderThread）
5. 编译通过，standalone test 加载 DLL 并看到清屏

### Phase 2: CommandBuffer + 基础场景操作
1. 双缓冲 `CommandBuffer` 实现
2. `ECMD_LOAD_SCENE`、`ECMD_SELECT_ENTITY`、`ECMD_SET_TRANSFORM`
3. `EntityHandle` 映射 + 查询接口
4. 回调系统：`OnSceneLoaded`、`OnEntitySelected`、`OnEntityListChanged`

### Phase 3: PlayMode + 物理 + Gizmo
1. `ECMD_PLAY_MODE` / `ECMD_STOP_MODE` + 场景快照
2. 输入事件转发（`ECMD_INPUT_KEY` / `ECMD_INPUT_MOUSE`）
3. Core 内部绘制 Viewport Toolbar + ImGuizmo
4. Gizmo 操作通过 `ECMD_GIZMO_MANIPULATE` 写回 Transform

### Phase 4: Inspector + 资产
1. 反射桥接：`Core_GetComponentProperty` / `Core_SetComponentProperty`
2. `ECMD_IMPORT_ASSET` + `AssetManager` 集成
3. 日志转发：`Core_GetLogMessages`
4. 完整 Editor ↔ Core 闭环验证

---

## 8. 风险与缓解

| 风险 | 说明 | 缓解方案 |
|------|------|---------|
| `RenderContext` RenderThread 冲突 | Core 默认启动渲染线程，WPF 也要求独立线程调用 | SyncMode：不启动 RenderThread，Editor 线程直接驱动 backend |
| 外部 HWND + Vulkan Surface | `vkCreateWin32SurfaceKHR` + HWND，替代 glfwCreateWindowSurface | Phase 1 先验证 OpenGL（wgl 更简单），Vulkan 跟进 |
| `dynamic_cast` 与 `-fno-rtti` | `World::get_system<T>()` 用了 `dynamic_cast` | 替换为 typeid hash 比较 + `static_cast`，或编译期索引表 |
| C# P/Invoke payload 对齐 | C struct payload 在 C# 侧需要精确内存布局 | payload 用固定大小数组 + `StructLayout(LayoutKind.Sequential, Pack = 1)` |
| Standalone examples 兼容性 | examples 目前直接 include core 内部头文件 | 逐步迁移 examples 也走 C API，或保留 static 链接模式 |

---

## 9. 立即行动

1. **确认本方案** → 开始 Phase 0：创建 `gryce_core.h` + 修改 CMake 输出 DLL
2. 如有调整（如回调设计、命令 payload 结构），现在提出
