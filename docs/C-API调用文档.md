# Gryce Engine C API 调用文档

> 本文档描述 Gryce Engine 面向宿主程序（编辑器、脚本、外部工具）公开的纯 C API。所有函数均为 `extern "C"`、`cdecl` 调用约定，通过 4 个模块 DLL 导出：

| DLL | 头文件目录 | 职责 |
|---|---|---|
| `GryceCore.dll` | `core/GryceCore/` | 生命周期、命令队列、场景/实体/组件、反射属性、资源、材质、动画、日志、回调 |
| `GrycePlatform.dll` | `core/GrycePlatform/` | 窗口（自建 GLFW 或外部 HWND）、输入、光标 |
| `GryceRenderer.dll` | `core/GryceRenderer/` | 渲染上下文、OpenGL/Vulkan 后端、视口/GameView 纹理 |
| `GrycePhysics.dll` | `core/GrycePhysics/` | Jolt（3D）/ Box2D（2D）物理世界、刚体、射线检测 |

头文件清单：

```text
core/GryceCore/types.h            # 共享基础类型（所有模块依赖）
core/GryceCore/core_api.h         # GCore_*：初始化、帧、命令队列、回调、日志
core/GryceCore/entity_api.h       # GEntity_*：实体查询、变换、Prefab、JSON 导入导出
core/GryceCore/component_api.h    # GComponent_*：组件增删、反射属性读写
core/GryceCore/scene_api.h        # GScene_*：场景加载/保存/新建/拾取
core/GryceCore/asset_api.h        # GAsset_*：资源导入/加载/卸载
core/GryceCore/material_api.h     # GMaterial_*：PBR 材质字段读写
core/GryceCore/animator_api.h     # GAnimator_*：动画片段查询
core/GrycePlatform/window_api.h   # GWindow_*：窗口生命周期与 GL 上下文
core/GrycePlatform/input_api.h    # GInput_*：输入注入与查询
core/GryceRenderer/render_api.h   # GRender_*：渲染生命周期与帧驱动
core/GryceRenderer/viewport_api.h # GViewport_* / GGameView_*：视口尺寸与相机
core/GrycePhysics/physics_api.h   # GPhysics_*：物理初始化与刚体操作
```

---

## 1. 通用约定

### 1.1 句柄

| 类型 | 语义 | 无效值 |
|---|---|---|
| `GEntityHandle` | 实体句柄（`int`，由引擎内部映射到 UUID） | `0` |
| `GComponentHandle` | 组件句柄（保留） | `0` |
| `GAssetHandle` | 资源句柄（`int`） | `-1` / `<= 0` |
| `GTextureHandle` | 渲染纹理指针（`void*`） | `NULL` |
| `GWindowHandle` | 窗口句柄（`void*`，Windows 下为 HWND） | `NULL` |
| `GBodyHandle` | 物理刚体句柄（`int`） | `0` |

实体句柄不是指针，不能跨场景保存使用：场景加载/新建后句柄表会重建（`rebuild`），旧句柄可能失效。

### 1.2 字符串缓冲区

输出字符串的函数采用 `(char* out_buf, int buf_size)` 模式：

- 成功返回**实际写入的字符数**（不含结尾 `'\0'`），并保证 `out_buf[buf_size-1] = '\0'`；
- 参数非法或实体无效返回 `-1`；
- 缓冲区不足时行为因函数而异（`GEntity_ExportJson` 返回 `-1`，其余函数截断）。

所有字符串按 **UTF-8** 编码（路径、实体名、组件名、日志）。

### 1.3 返回码约定

- `int` 返回 `0` 表示成功，`-1` 表示失败；
- 句柄返回 `0` / `NULL` 表示无效或未命中；
- `bool` 返回按函数语义；
- `GCoreInitDesc.version` / `GRenderInitDesc.version` 必须等于对应结构体的 `sizeof`（用 `sizeof(desc)` 或 `Marshal.SizeOf<T>()` 填充），否则初始化失败。

### 1.4 线程模型

- 引擎内部对**所有导出函数**使用一个全局递归互斥锁（`GRYCE_API_GUARD()`）保护，场景数据不会在渲染管线迭代时被并发修改；
- 递归锁允许同一线程内 API 互相调用、以及回调中再次调用 API（编辑器回调运行在 UI 线程）；
- 建议保持现有编辑器模式：UI 线程负责命令下发与 60Hz 帧驱动，专用渲染线程负责 GL 上下文与 `GRender_*` 调用。

### 1.5 初始化失败检查

绝大多数函数在未初始化时是安全的空操作（返回 `-1` / `0` / `false`）。调用前可用 `GCore_IsInitialized()`、`GRender_IsInitialized()`、`GWindow_IsValid()` 检查状态。

---

## 2. 初始化与生命周期

### 2.1 推荐初始化顺序

```text
1. GCore_Init(&desc)                    # 创建 World、注册内置组件与核心系统
2. GCore_GetInternalWorldPtr()          # 取内部 World 指针
3. GPhysics_Init(Jolt)                  # 初始化物理后端（3D + 2D）
4. GPhysics_AttachSystems(worldPtr)     # 把 PhysicsSystem3D/2D 注册进 World
5. GWindow_InitExternal(hwnd, w, h)     # 外部 HWND（编辑器）或 GWindow_Create(...)
6. GRender_Init(&desc)                  # 创建渲染后端与管线
7. GCore_RegisterCallback_*(...)        # 注册回调（可在 Init 之后任意时刻）
```

> 物理系统必须**先于** Play Mode 使用前注册；`GCore_Init` 已内置注册 `AnimatorSystem`、`FractureSystem`、`SubViewportSystem`。

### 2.2 每帧调用时序（编辑器同步模式）

```text
UI 线程（60Hz）：
    GCore_BeginFrame(dt)    # 交换命令缓冲、执行命令（每帧最多 30 条）、Play Mode 下 world.update(dt)
    GCore_EndFrame()        # 转发日志、触发延迟回调（OnSceneLoaded / OnEntityListChanged）

渲染线程（垂直同步）：
    GWindow_MakeContextCurrent()
    GViewport_SetSize(w, h)   # 窗口尺寸变化时
    GRender_BeginFrame()
    GRender_RenderWorld()
    GRender_EndFrame()        # 在锁外执行 swap，避免 VSync 阻塞 UI 线程
    GWindow_ReleaseContext()
```

异步模式（demo，非编辑器）：`GCore_*` 与 `GRender_*` 由同一主循环驱动，`GRender_Init` 内部启动渲染线程。

### 2.3 关闭顺序

```text
1. GRender_Shutdown()
2. GWindow_Destroy()
3. GPhysics_Shutdown()
4. GCore_Shutdown()
```

---

## 3. 命令队列（Command Buffer）

Editor 对场景的**结构性修改**（创建/销毁实体、改属性、Play Mode 等）通过命令下发，由 Core 在 `GCore_BeginFrame` 中消费；直接查询类 API（`GEntity_*`、`GComponent_*` 等）则同步执行。

### 3.1 结构

```c
typedef enum { /* 见下表 */ } GCommandType;

#define GCMD_PAYLOAD_SIZE 256

typedef struct {
    GCommandType type;          // 命令类型
    uint64_t     seq;           // 序号（当前未使用）
    uint8_t      payload[256];  // 载荷
} GCommand;
```

- 双缓冲（double-buffer）队列，每缓冲容量 **8192** 条；
- 生产端（Editor 线程）无锁写入，消费端（Core 线程）在 `GCore_BeginFrame` 中交换并消费；
- **每帧最多处理 30 条命令**，超出部分自动回队到下一帧（不丢失）；
- `GCore_PushCommands` 返回被丢弃的条数（队列满时），`GCore_GetDroppedCmdCount` 可累计查询。

### 3.2 命令类型与载荷布局

| 枚举 | 值 | 载荷（`payload` 内存布局） | 当前处理 |
|---|---|---|---|
| `ECMD_LOAD_SCENE` | 1 | `char path[256]`（C 字符串） | ✅ |
| `ECMD_SAVE_SCENE` | 2 | — | ❌ 未实现 |
| `ECMD_CREATE_ENTITY` | 3 | `struct { char name[128]; GEntityHandle parent; }` | ✅ |
| `ECMD_DESTROY_ENTITY` | 4 | `GEntityHandle h` | ✅ |
| `ECMD_RENAME_ENTITY` | 5 | `struct { GEntityHandle h; char name[128]; }` | ✅ |
| `ECMD_REPARENT_ENTITY` | 6 | `struct { GEntityHandle h; GEntityHandle parent; }`（parent=0 表示场景根） | ✅ |
| `ECMD_SELECT_ENTITY` | 7 | `GEntityHandle h` | ✅ |
| `ECMD_SET_TRANSFORM` | 8 | `struct { GEntityHandle h; GVec3 pos; GQuat rot; GVec3 scale; }` | ✅ |
| `ECMD_SET_PROPERTY` | 9 | `struct { GEntityHandle h; uint64_t type_hash; char prop_name[64]; uint8_t value[128]; }` | ✅ |
| `ECMD_ADD_COMPONENT` | 10 | `struct { GEntityHandle h; char type_name[128]; }` | ✅ |
| `ECMD_REMOVE_COMPONENT` | 11 | `struct { GEntityHandle h; uint64_t type_hash; }` | ✅ |
| `ECMD_PLAY_MODE` | 12 | 无 | ✅（进入前保存场景快照） |
| `ECMD_STOP_MODE` | 13 | 无 | ✅（恢复快照，丢弃播放期修改） |
| `ECMD_PAUSE_MODE` | 14 | 无 | ✅（切换暂停/恢复） |
| `ECMD_STEP_FRAME` | 15 | 无 | ❌ 未实现 |
| `ECMD_IMPORT_ASSET` | 16 | — | ❌ 未实现 |
| `ECMD_SET_RENDER_TARGET` | 100 | — | ❌ 未实现（枚举预留） |
| `ECMD_SET_VIEWPORT_SIZE` | 101 | — | ❌ 未实现 |
| `ECMD_SET_GAMEVIEW_SIZE` | 102 | — | ❌ 未实现 |
| `ECMD_SET_MATERIAL` | 103 | — | ❌ 未实现 |
| `ECMD_INPUT_KEY` | 200 | — | ❌ 未实现（改用 `GInput_Inject*`） |
| `ECMD_INPUT_MOUSE_MOVE` | 201 | — | ❌ |
| `ECMD_INPUT_MOUSE_BUTTON` | 202 | — | ❌ |
| `ECMD_INPUT_MOUSE_SCROLL` | 203 | — | ❌ |
| `ECMD_PHYSICS_SET_GRAVITY` | 300 | — | ❌ 未实现（改用 `GPhysics_SetGravity`） |
| `ECMD_PHYSICS_ADD_FORCE` | 301 | — | ❌ |
| `ECMD_GIZMO_SET_OPERATION` | 400 | — | ❌ 未实现 |
| `ECMD_GIZMO_SET_SPACE` | 401 | — | ❌ |
| `ECMD_GIZMO_MANIPULATE` | 402 | — | ❌ |

> 说明：类型哈希 `type_hash` 为组件类型名（如 `"MeshRenderer"`）的 `std::hash<std::string>` 结果，与 `GComponent_GetTypeHashAt` 返回一致。编辑器可先用 `GComponent_GetRegisteredTypeInfo` 枚举已注册类型。

### 3.3 命令示例（C#）

```csharp
// 创建实体并挂到 parent 下
unsafe
{
    var payload = new byte[128 + sizeof(int)];
    Encoding.UTF8.GetBytes("MyEntity").CopyTo(payload, 0);
    BitConverter.GetBytes((int)parent).CopyTo(payload, 128);
    var cmd = GCommand.Create(GCommandType.CreateEntity, payload);
    CoreAPI.GCore_PushCommand(ref cmd);
}
```

---

## 4. 回调

回调通过 `GCore_RegisterCallback_*` 注册，均为 C 函数指针；C# 侧需要把委托字段保持存活（防止 GC 回收），并在 `GOnLogMessage` 等委托上标注 `[UnmanagedFunctionPointer(CallingConvention.Cdecl)]`。

| 回调 | 触发时机 |
|---|---|
| `GOnEntitySelected` | `ECMD_SELECT_ENTITY` 选中非 0 实体时（立即触发） |
| `GOnEntityDeselected` | 选中从 A 切换到 B 或清除时 |
| `GOnSceneLoaded` | 场景加载/新建成功后的下一帧（`GCore_EndFrame` 中延迟触发） |
| `GOnPlayModeChanged` | 播放/停止/暂停状态变化时 |
| `GOnEntityListChanged` | 实体增删/改名/重挂/导入等结构性变化后的下一帧 |
| `GOnComponentChanged` | 组件变化（当前在命令处理中暂未广泛触发） |
| `GOnLogMessage` | 每帧 `GCore_EndFrame` 统一转发引擎日志 |
| `GOnViewportTextureReady` | 视口纹理就绪（保留，尚未接入） |

`GCore_SetCallback_UserData(void*)` 设置回调的 `user_data` 参数（C# 侧通常传入 `GCHandle` 或 `nint` 包装的托管对象指针）。

---

## 5. API 参考

### 5.1 GryceCore — 生命周期与帧驱动（`core_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GCore_Init(const GCoreInitDesc* desc)` | 初始化核心：创建 World、注册内置组件、注册 AnimatorSystem/FractureSystem/SubViewportSystem，创建默认 "Untitled" 场景 | `0` 成功；`-1` desc 为空或 version 不匹配；重复调用返回 `0` |
| `void GCore_Shutdown(void)` | 关闭 World 并清空状态 | — |
| `bool GCore_IsInitialized(void)` | 是否已初始化 | — |
| `void GCore_BeginFrame(float dt)` | 交换并消费命令队列（≤30 条/帧）；Play Mode 且未暂停时 `world.update(dt)` | — |
| `void GCore_EndFrame(void)` | 转发日志、触发延迟回调 | — |
| `int GCore_PushCommand(const GCommand* cmd)` | 压入单条命令（线程安全） | `0` 成功；`-1` 参数无效或未初始化 |
| `int GCore_PushCommands(const GCommand* cmds, int count)` | 批量压入 | 返回丢弃条数；`-1` 参数无效 |
| `int GCore_GetCmdQueueCapacity(void)` | 当前队列剩余容量 | ≥ 0 |
| `int GCore_GetDroppedCmdCount(void)` | 自上次调用以来丢弃的命令数 | ≥ 0 |
| `bool GCore_IsPlaying(void)` / `bool GCore_IsPaused(void)` | Play Mode / 暂停状态 | — |
| `void GCore_SetCallback_UserData(void* user_data)` | 设置回调 user_data | — |
| `void GCore_RegisterCallback_On*(cb)` | 注册各类回调（见第 4 节） | — |
| `int GCore_GetLogMessages(char* out_buf, int buf_size)` | 拉取内存日志（换行分隔） | 写入字节数；`-1` 参数无效 |
| `GPackHandle GCore_PackCreate(void)` | 创建 GPack 资源包写入器（GryceGC 使用；格式与 `GPackReader` 一致） | 句柄；`NULL` 失败 |
| `int GCore_PackAddFile(GPackHandle h, const char* internal_path, const char* source_path)` | 将 `source_path` 文件加入包内 `internal_path`（正斜杠、相对项目根） | `0` 成功；`-1` 失败 |
| `int GCore_PackWrite(GPackHandle h, const char* output_path)` | 写出 `.gpkg`/`.gpack` 文件 | `0` 成功；`-1` 失败 |
| `void GCore_PackDestroy(GPackHandle h)` | 释放写入器；可传 `NULL` | — |
| `void* GCore_GetInternalWorldPtr(void)` | 内部 `ecs::World*`（仅供同进程其他模块 DLL 使用，如 `GPhysics_AttachSystems`） | 指针或 `NULL` |

`GCoreInitDesc`：

```c
typedef struct {
    uint32_t version;           // 必须 == sizeof(GCoreInitDesc)
    const char* project_root;   // res:/ 虚拟路径的根（真实文件系统路径，不能传 "res:/"）
    bool enable_reflection;     // 是否启用反射（编辑器传 true）
} GCoreInitDesc;
```

### 5.2 GryceCore — 实体（`entity_api.h`）

#### 层级查询

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GEntity_GetCount(void)` | 实体总数（**不含**场景合成根节点） | ≥ 0 |
| `GEntityHandle GEntity_GetAt(int index)` | 按遍历序取实体 | 句柄；越界返回 `0` |
| `int GEntity_GetName(GEntityHandle e, char* out, int size)` | 实体名 | 字符数；`-1` 失败 |
| `int GEntity_GetPath(GEntityHandle e, char* out, int size)` | 从根到自身的 `A/B/C` 路径（不含场景根名） | 字符数；`-1` 失败 |
| `GEntityHandle GEntity_GetParent(GEntityHandle e)` | 父实体；根级实体返回 `0` | 句柄 |
| `int GEntity_GetChildCount(GEntityHandle e)` | 子实体数 | ≥ 0 |
| `GEntityHandle GEntity_GetChildAt(GEntityHandle e, int index)` | 第 index 个子实体 | 句柄；越界返回 `0` |
| `int GEntity_GetSiblingIndex(GEntityHandle e)` | 兄弟索引 | ≥ 0；`-1` 失败 |
| `GEntityHandle GEntity_GetSelected(void)` | 当前选中实体 | 句柄（无选中为 `0`） |

#### 变换

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GEntity_GetLocalPosition(GEntityHandle e, GVec3* out)` | 局部位置 | `0` / `-1` |
| `int GEntity_GetLocalRotation(GEntityHandle e, GQuat* out)` | 局部旋转（四元数 xyzw） | `0` / `-1` |
| `int GEntity_GetLocalScale(GEntityHandle e, GVec3* out)` | 局部缩放 | `0` / `-1` |
| `int GEntity_SetLocalPosition/Rotation/Scale(...)` | 写入局部变换（写入后实体标记 dirty） | `0` / `-1` |
| `int GEntity_GetWorldPosition/Rotation/Scale(...)` | **未实现**，恒返回 `-1` | `-1` |

#### 导出 / 导入 / Prefab

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GEntity_ExportJson(GEntityHandle e, char* out, int size)` | 导出实体子树为 JSON（扁平 `entities` 数组，根 `parent=null`；用于 Undo/Redo、剪贴板、Prefab 创建） | 字节数；缓冲区不足返回 `-1` |
| `GEntityHandle GEntity_ImportJson(const char* json, GEntityHandle parent)` | 从 ExportJson 输出导入子树（生成全新 UUID/ID；parent=0 挂到场景根） | 新子树根句柄；失败 `0` |
| `int GEntity_SaveAsPrefab(GEntityHandle e, const char* path)` | 保存实体子树为 Prefab（`.gesc` / `.geprefab`） | `0` / `-1` |
| `GEntityHandle GEntity_CreatePrefabInstance(const char* path, GEntityHandle parent)` | 实例化 Prefab（自动挂 `PrefabInstance` 组件） | 新实例根句柄；失败 `0` |
| `int GEntity_ApplyPrefab(GEntityHandle e)` | 把实例当前状态写回模板（Apply） | `0` / `-1` |
| `int GEntity_RevertPrefab(GEntityHandle e)` | 还原实例为模板 + 覆盖参数状态（Revert） | `0` / `-1` |

### 5.3 GryceCore — 组件（`component_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GComponent_GetCount(GEntityHandle e)` | 实体组件数 | ≥ 0 |
| `int GComponent_GetTypeHashAt(GEntityHandle e, int index, uint64_t* out)` | 第 index 个组件的类型哈希 | `0` / `-1` |
| `int GComponent_GetTypeNameAt(GEntityHandle e, int index, char* out, int size)` | 第 index 个组件的类型名 | 字符数 / `-1` |
| `int GComponent_GetPropertyCount(GEntityHandle e, uint64_t hash)` | 反射属性数 | ≥ 0 |
| `int GComponent_GetPropertyInfo(GEntityHandle e, uint64_t hash, int idx, char* name, int nsize, int* type, int* size)` | 属性名/类型/字节大小 | `0` / `-1` |
| `int GComponent_GetProperty(GEntityHandle e, uint64_t hash, const char* name, void* out, int size)` | 读取属性（字符串字段写入 `std::string` 对象的内存，见下） | `0` / `-1` |
| `int GComponent_SetProperty(GEntityHandle e, uint64_t hash, const char* name, const void* value, int size)` | 写入属性（只读字段被忽略） | `0` / `-1` |
| `int GComponent_AddComponent(GEntityHandle e, uint64_t hash)` | 添加组件（需通过工厂注册的类型） | `0` / `-1` |
| `int GComponent_RemoveComponent(GEntityHandle e, uint64_t hash)` | 移除组件 | `0` / `-1` |
| `int GComponent_GetRegisteredTypeCount(void)` | 已注册组件类型总数 | ≥ 0 |
| `int GComponent_GetRegisteredTypeInfo(int idx, uint64_t* out_hash, char* out_name, int size)` | 枚举已注册类型（名称 + 哈希） | `0` / `-1` |

> **字符串属性**：反射的字符串字段内部为 `std::string`。通过 `GComponent_SetProperty` 写入时传入 `\0` 结尾的 C 字符串指针；读取时缓冲区需容纳 `std::string` 对象本身，实际使用中建议优先走 `ECMD_SET_PROPERTY`（载荷值按 C 字符串解释）或 `GMaterial_SetField`（材质专用）。

### 5.4 GryceCore — 场景（`scene_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GScene_Load(const char* path)` | 加载 `.gesc` 场景（同步，`res:/` 路径可解析） | `0` / `-1` |
| `int GScene_Save(const char* path)` | 保存当前场景 | `0` / `-1` |
| `int GScene_GetCurrentPath(char* out, int size)` | 当前场景路径（未保存过则空串） | 字符数 / `-1` |
| `int GScene_New(void)` | 新建空场景 "Untitled" | `0` / `-1` |
| `GEntityHandle GScene_PickScreen(float sx, float sy, int vp_w, int vp_h, GEntityHandle camera)` | 屏幕坐标拾取（左上原点、Y 向下）；对 MeshRenderer/SkinnedMeshRenderer 做世界 AABB 求交；camera=0 时自动找第一个启用的 Camera | 最近命中实体；未命中 `0` |
| `GEntityHandle GScene_PickRay(const GVec3* origin, const GVec3* dir, float max_dist)` | 世界空间射线拾取（dir 无需归一化；max_dist<=0 不限距离） | 命中实体；未命中 `0` |

### 5.5 GryceCore — 资源（`asset_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `GAssetHandle GAsset_Import(const char* path)` | 导入资源（加载 mesh 并登记句柄） | 句柄（>0）；失败 `-1` |
| `GAssetHandle GAsset_Load(const char* path)` | 加载资源（等价于 Import，语义保留） | 句柄；失败 `-1` |
| `int GAsset_GetPath(GAssetHandle h, char* out, int size)` | 资源路径 | 字符数 / `-1` |
| `void GAsset_Unload(GAssetHandle h)` | 卸载资源（释放 AssetManager 引用并移除句柄） | — |

### 5.6 GryceCore — 材质（`material_api.h`）

作用于实体上的 `MeshRenderer` / `SkinnedMeshRenderer` 组件的 PBR 材质。

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GMaterial_GetField(GEntityHandle e, uint64_t hash, int field, float* out_floats, int fc, char* out_str, int sc)` | 读字段：标量/向量写入 `out_floats`，字符串字段写入 `out_str` | `0` / `-1` |
| `int GMaterial_SetField(GEntityHandle e, uint64_t hash, int field, const float* in_floats, int count, const char* in_str)` | 写字段；修改贴图路径/use 标志会把材质标记 `textures_dirty`，渲染线程下一帧自动重传贴图 | `0` / `-1` |
| `int GMaterial_LoadFromFile(GEntityHandle e, uint64_t hash, const char* path)` | 从 `.gmat` 加载材质到渲染组件 | `0` / `-1` |

`GMaterialField` 枚举：

| 字段 | 类型 | 说明 |
|---|---|---|
| `GMAT_ALBEDO_COLOR` | `float[3]` | 基础颜色 RGB |
| `GMAT_ROUGHNESS` / `GMAT_METALLIC` / `GMAT_AO` | `float` | 粗糙度 / 金属度 / 环境光遮蔽 |
| `GMAT_EMISSIVE_COLOR` | `float[3]` | 自发光 RGB |
| `GMAT_OPACITY` | `float` | 不透明度 |
| `GMAT_BLEND_MODE` | `int` | 0=Opaque, 1=Blend |
| `GMAT_TWO_SIDED` | `bool` | 双面渲染 |
| `GMAT_UV_SCALE` / `GMAT_UV_OFFSET` | `float[2]` | UV 缩放 / 偏移 |
| `GMAT_ALBEDO_MAP_PATH` 等 6 个 `*_MAP_PATH` | `string` | 贴图路径 |
| `GMAT_USE_*_MAP` 6 个 | `bool` | 是否启用对应贴图 |

### 5.7 GryceCore — 动画（`animator_api.h`）

作用于 `SkinnedMeshRenderer`（查询模型自带动画片段）。

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GAnimator_GetClipCount(GEntityHandle e, uint64_t hash)` | 动画片段数 | ≥ 0；`-1` 实体/组件无效 |
| `int GAnimator_GetClipName(GEntityHandle e, uint64_t hash, int index, char* out, int size)` | 片段名 | 字符数；`-1` 失败 |
| `float GAnimator_GetClipDuration(GEntityHandle e, uint64_t hash, int index)` | 片段时长（秒） | 时长；`-1` 失败 |

### 5.8 GrycePlatform — 窗口（`window_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GWindow_InitExternal(GWindowHandle hwnd, int w, int h)` | 绑定外部窗口（WPF HwndHost 提供 HWND；不创建 GLFW 窗口） | `0` / `-1` |
| `int GWindow_Create(const char* title, int w, int h, GWindowMode mode)` | 自建窗口（独立 demo 用） | `0` / `-1` |
| `void GWindow_Destroy(void)` | 销毁窗口 | — |
| `bool GWindow_IsValid(void)` | 窗口是否有效 | — |
| `void GWindow_GetSize(int* w, int* h)` | 窗口尺寸 | — |
| `void GWindow_SetSize(int w, int h)` | 调整尺寸 | — |
| `GWindowHandle GWindow_GetNativeHandle(void)` | 原生句柄（HWND / GLFWwindow*） | — |
| `GWindowHandle GWindow_GetRenderHandle(void)` | 渲染可用句柄（外部模式下为嵌入的 GLFW 子窗口） | — |
| `bool GWindow_ShouldClose(void)` | 是否应关闭 | — |
| `void GWindow_PollEvents(void)` | 轮询事件（GLFW） | — |
| `void GWindow_SwapBuffers(void)` | 交换缓冲 | — |
| `void GWindow_MakeContextCurrent(void)` | 让 GL 上下文在当前线程生效（编辑器渲染线程用） | — |
| `void GWindow_ReleaseContext(void)` | 释放当前线程的 GL 上下文 | — |
| `void GWindow_SetSwapInterval(int interval)` | 交换间隔（VSync；1=开，0=关） | — |

### 5.9 GrycePlatform — 输入（`input_api.h`）

| 函数 | 说明 |
|---|---|
| `void GInput_InjectKey(int key_code, GInputAction action)` | 注入按键（key_code 使用 GLFW 键码） |
| `void GInput_InjectMouseMove(float x, float y)` | 注入鼠标位置 |
| `void GInput_InjectMouseButton(int button, GInputAction action, float x, float y)` | 注入鼠标按钮 |
| `void GInput_InjectMouseScroll(float dx, float dy)` | 注入滚轮 |
| `bool GInput_IsKeyPressed(int key_code)` | 按键是否在本帧按下（边沿） |
| `bool GInput_IsKeyHeld(int key_code)` | 按键是否持续按住（电平） |
| `bool GInput_IsMouseButtonPressed(int button)` | 鼠标按钮边沿 |
| `void GInput_GetMousePosition(float* x, float* y)` | 当前鼠标位置 |

`GInputAction`：`GINPUT_ACTION_PRESS = 0`、`GINPUT_ACTION_RELEASE`、`GINPUT_ACTION_REPEAT`。

### 5.10 GryceRenderer — 渲染（`render_api.h`）

```c
typedef struct {
    uint32_t     version;         // 必须 == sizeof(GRenderInitDesc)
    GWindowHandle native_window;  // 窗口句柄（或 GWindow_GetRenderHandle() 的结果）
    GRenderAPI   api;             // GRYCE_RENDER_API_OPENGL / VULKAN
    int          viewport_w;      // 视口宽（>0）
    int          viewport_h;      // 视口高
    bool         sync_mode;       // true=编辑器同步模式（无内部渲染线程）
} GRenderInitDesc;
```

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GRender_Init(const GRenderInitDesc* desc)` | 创建后端、RenderContext、RenderPipeline（失败时 fallback 到仅清屏） | `0` / `-1` |
| `void GRender_Shutdown(void)` | 关闭管线与上下文 | — |
| `bool GRender_IsInitialized(void)` | 是否已初始化 | — |
| `void GRender_BeginFrame(void)` | 同步模式下调用后端 `begin_frame` | — |
| `void GRender_RenderWorld(void)` | 渲染当前 World（主相机 + 最多 8 盏灯；同步模式下先补传待上传网格，预算 30/帧） | — |
| `void GRender_RenderGizmo(void)` | **占位**（TODO：ImGuizmo + 工具栏） | — |
| `void GRender_RenderGameView(void)` | 渲染 GameView（当前与 SceneView 共用同一管线） | — |
| `void GRender_EndFrame(void)` | 同步模式下执行待处理命令并把 swap 移到锁外执行，避免 VSync 阻塞 | — |
| `GTextureHandle GRender_GetViewportTexture(void)` | 视口颜色纹理（管线关闭时返回 NULL） | — |
| `GTextureHandle GRender_GetGameViewTexture(void)` | GameView 纹理（当前即视口纹理） | — |
| `int GRender_GetViewportSize(int* w, int* h)` / `GetGameViewSize` | 尺寸查询 | `0` / `-1` |
| `void GRender_SetVSync(bool enabled)` | 设置交换间隔 1/0 | — |
| `void GRender_SetDisplayMode(const char* mode)` | 记录显示模式（"Shaded" 等；**尚未应用到后端**） | — |

### 5.11 GryceRenderer — 视口 / GameView（`viewport_api.h`）

| 函数 | 说明 |
|---|---|
| `void GViewport_SetSize(int w, int h)` / `GViewport_GetSize(int* w, int* h)` | 视口尺寸（Set 会同步 resize 管线渲染目标） |
| `void GViewport_SetCamera(GEntityHandle cam)` / `GViewport_GetCamera(void)` | 编辑器指定视口相机 |
| `void GGameView_SetSize(int w, int h)` / `GGameView_GetSize(int* w, int* h)` | GameView 尺寸（当前仅记录，未驱动独立 FBO） |
| `void GGameView_SetCamera(GEntityHandle cam)` | GameView 相机（当前记录） |

### 5.12 GrycePhysics — 物理（`physics_api.h`）

| 函数 | 说明 | 返回 |
|---|---|---|
| `int GPhysics_Init(GPhysicsBackend backend)` | 初始化物理后端（同时创建 3D 与 2D 世界；`JOLT=0` / `BOX2D=1`） | `0` / `-1` |
| `void GPhysics_Shutdown(void)` | 关闭物理世界 | — |
| `int GPhysics_AttachSystems(void* world_ptr)` | 把 `PhysicsSystem3D` + `PhysicsSystem2D` 注册进 Core World（参数来自 `GCore_GetInternalWorldPtr`） | `0` / `-1` |
| `void GPhysics_SetGravity(const GVec3* g)` | 设置 3D 重力 | — |
| `void GPhysics_Step(float dt, int substeps)` | 手动步进（substeps<=0 时按 1；2D 固定 8 子步 × 3 迭代） | — |
| `GBodyHandle GPhysics_CreateBody(GEntityHandle e, bool is_static)` | 创建刚体（当前**未绑定实体变换**，位置默认；由 PhysicsSystem 在 Play Mode 中重建） | 句柄；失败 `0` |
| `void GPhysics_DestroyBody(GBodyHandle b)` | 销毁刚体 | — |
| `void GPhysics_SetBodyTransform(GBodyHandle b, const GVec3* pos, const GQuat* rot)` | 设置刚体变换 | — |
| `void GPhysics_GetBodyTransform(GBodyHandle b, GVec3* pos, GQuat* rot)` | 读取刚体变换 | — |
| `void GPhysics_AddForce(GBodyHandle b, const GVec3* force)` | 施加力（质心） | — |
| `void GPhysics_AddImpulse(GBodyHandle b, const GVec3* impulse)` | 施加冲量 | — |
| `bool GPhysics_Raycast(const GVec3* o, const GVec3* d, float max_dist, GVec3* pt, GVec3* n, GEntityHandle* e)` | 3D 射线检测；命中填充点/法线；**`out_entity` 暂未映射，恒为 0** | `true` 命中 |

---

## 6. 调用示例

### 6.1 C 示例：最小编辑器宿主

```c
#include <GryceCore/core_api.h>
#include <GryceCore/entity_api.h>
#include <GryceCore/component_api.h>
#include <GryceCore/scene_api.h>
#include <GrycePlatform/window_api.h>
#include <GryceRenderer/render_api.h>
#include <GrycePhysics/physics_api.h>

static void on_scene_loaded(const char* path, void* user) {
    /* 场景加载完成，刷新 Hierarchy */
}

int main(void) {
    GCoreInitDesc core_desc = {
        .version = sizeof(GCoreInitDesc),
        .project_root = "C:/MyGame",   /* res:/ 的根，必须为真实路径 */
        .enable_reflection = true,
    };
    if (GCore_Init(&core_desc) != 0) return -1;

    void* world = GCore_GetInternalWorldPtr();
    GPhysics_Init(GPHYSICS_BACKEND_JOLT);
    GPhysics_AttachSystems(world);

    GCore_RegisterCallback_OnSceneLoaded(on_scene_loaded, NULL);
    GScene_Load("res:/scenes/main.gesc");

    /* 每帧：GCore_BeginFrame(dt) ... GCore_EndFrame() */
    for (;;) {
        GCore_BeginFrame(1.0f / 60.0f);
        GCore_EndFrame();
    }

    GCore_Shutdown();
    return 0;
}
```

### 6.2 C# 示例：编辑器集成（与 `editor/src/` 一致）

```csharp
// 初始化（顺序与 2.1 节一致）
var coreDesc = new GCoreInitDesc {
    Version = (uint)Marshal.SizeOf<GCoreInitDesc>(),
    ProjectRoot = @"C:\MyGame",
    EnableReflection = true
};
CoreAPI.GCore_Init(ref coreDesc);

var worldPtr = CoreAPI.GCore_GetInternalWorldPtr();
PhysicsAPI.GPhysics_Init(GPhysicsBackend.Jolt);
PhysicsAPI.GPhysics_AttachSystems(worldPtr);

// 每 60Hz 驱动核心
CoreAPI.GCore_BeginFrame(dt);
CoreAPI.GCore_EndFrame();

// 枚举实体并读取名称
int count = EntityAPI.GEntity_GetCount();
for (int i = 0; i < count; i++)
{
    var handle = EntityAPI.GEntity_GetAt(i);
    var name = EntityAPI.GetNameUtf8(handle);   // StringBuilder 包装的 UTF-8 读取
}

// 创建实体（命令方式，下一帧生效）
EngineService.PushCommand(GCommandType.CreateEntity, payload);
```

> 完整的 P/Invoke 声明见 `editor/src/Native/`（`CoreAPI.cs`、`EntityAPI.cs`、`RenderAPI.cs` 等），其中 Debug 构建使用带 `d` 后缀的 DLL（`GryceCored.dll` 等）。

---

## 7. 已知 API 缺口（当前版本）

| 缺口 | 说明 |
|---|---|
| `GEntity_GetWorldPosition/Rotation/Scale` | 未实现，恒返回 `-1` |
| `GRender_RenderGizmo` / `GRender_SetDisplayMode` | 占位实现（仅记录状态） |
| `GRender_RenderGameView` / `GGameView_*` | 与 SceneView 共用管线/纹理，无独立 FBO |
| `GPhysics_CreateBody` 实体绑定 | 未从实体 Transform 同步初始位置（由 PhysicsSystem 在 Play Mode 负责重建） |
| `GPhysics_Raycast` 实体映射 | `out_entity` 恒为 `0` |
| `ECMD_*` 部分命令 | `SAVE_SCENE`、`STEP_FRAME`、`IMPORT_ASSET`、`SET_MATERIAL`、输入/物理/Gizmo 命令仅枚举未处理 |
| `GOnViewportTextureReady` | 回调类型已定义，尚未接入 |
| DirectX 11/12 | 枚举预留，未实现 |

以上缺口随引擎迭代逐步补齐，本文档会同步更新。
