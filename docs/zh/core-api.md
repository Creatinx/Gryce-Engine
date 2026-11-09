# GryceCore 接口手册（core-api）

本文是 `GryceCore.dll` 对外 **全部** C 接口的逐函数说明：每个函数给出签名、作用、参数与返回值语义、以及可直接编译的调用示例。共 8 个模块、138 个导出函数。

| 模块       | 头文件                          | 函数数 | 职责                        |
| -------- | ---------------------------- | --- | ------------------------- |
| 核心 / 帧循环 | `engine/api/core_api.h`      | 36  | 初始化、帧边界、命令队列、日志、撤销重做、资源打包 |
| 实体       | `engine/api/entity_api.h`    | 24  | 层级、变换、选择、JSON 导入导出、Prefab |
| 组件       | `engine/api/component_api.h` | 29  | 组件枚举、反射属性、组件树、类型继承        |
| 场景       | `engine/api/scene_api.h`     | 10  | 场景载入/保存、拾取、2D/3D 双槽       |
| 资产       | `engine/api/asset_api.h`     | 10  | 资产注册、侧车（.meta）、目录遍历       |
| 材质       | `engine/api/material_api.h`  | 3   | PBR 字段级读写、.gmat 载入        |
| 动画       | `engine/api/animator_api.h`  | 10  | 动画片段 / 骨骼 / 轨道 / 关键帧查询    |
| 编辑命令面    | `engine/api/edit_api.h`      | 16  | 按语义封装的写入命令（唯一改场景的通道）      |

相关文档：[integration.md](integration.md)（接入宿主）、[scripting.md](scripting.md)（脚本层）、[app-entry.md](app-entry.md)（固定宿主）。

---

## 0. 通用约定

### 0.1 链接

`GryceCore` 是共享库（WebGL 构建为静态库）。Windows + MinGW 使用 `build/lib/libGryceCore.dll.a` 导入库；MSVC 使用 `build/lib/GryceCore.lib`。头文件根为 `engine/`，包含方式：

```c
#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/component_api.h"
#include "api/scene_api.h"
#include "api/asset_api.h"
#include "api/material_api.h"
#include "api/animator_api.h"
#include "api/edit_api.h"
```

所有符号都是 `extern "C"`，可被 C、C++、Rust、C#（P/Invoke）、Python（ctypes）直接调用，不需要任何 C++ ABI 兼容。

### 0.2 句柄

| 类型                 | 含义                   | 无效值    |
| ------------------ | -------------------- | ------ |
| `GEntityHandle`    | 实体 id（进程内单调分配，不随帧变化） | `0`    |
| `GComponentHandle` | 组件句柄（预留，当前未导出使用）     | `0`    |
| `GAssetHandle`     | 资产注册 id              | `<= 0` |
| `GTextureHandle`   | 渲染器纹理句柄（不透明指针）       | `NULL` |
| `GWindowHandle`    | 平台窗口句柄（不透明指针）        | `NULL` |

实体句柄在**场景被替换**后失效（`GScene_Load` / `GScene_New` / `GCore_Shutdown`）。失效句柄的所有查询返回 0 / -1，不崩溃。

### 0.3 返回值口径

引擎没有异常跨 DLL 边界，统一用返回值表达：

- `int` 型查询：成功返回 `0`，失败返回 `-1`（少数"数量"语义函数直接返回数量）。
- 布尔型：`1` 真、`0` 假。
- 写入类命令（`GEdit_*`、`GCore_PushCommand`）：返回值只表示**是否入队**，不代表执行结果；执行期的失败写日志。
- 字符串输出：返回写入长度（不含结尾 `\0`），失败 `-1`；缓冲区不足时字符串被截断且保证 `\0` 结尾。

### 0.4 线程与锁

每个导出函数入口都会取一把**进程级递归互斥锁**（`gryce_core::api_mutex()`），因此：

- 多个线程可以同时调用，不会数据竞争；
- 回调（见 1.13）在持锁状态下触发，回调内**禁止**再调用写类接口（会重入同一线程的锁，虽然递归锁不死锁，但会破坏帧边界语义）；
- 渲染线程与 UI 线程的分工见 [integration.md](integration.md) 第 4 节。

### 0.5 字符串路径

所有路径参数同时接受两种形态：

- `res:/models/cube.obj` —— 相对项目根（由 `GCore_Init` 的 `project_root` 决定），跨机器可移植，推荐；
- 绝对路径 / 相对当前工作目录的路径 —— 直接交给文件系统。

---

## 1. 核心与帧循环（core_api.h）

### 1.1 `GCore_Init`

```c
int GCore_Init(const GCoreInitDesc* desc);
```

作用：初始化引擎核心（日志、反射表、World、场景、脚本运行时、物理与音频子系统）。全进程只需调用一次，重复调用返回 0 且不做任何事。

参数：

```c
typedef struct {
    uint32_t    version;          // 必须填 sizeof(GCoreInitDesc)，用于 ABI 校验
    const char* project_root;     // 项目根目录（res:/ 的解析基准），可为 NULL
    bool        enable_reflection;// 是否注册反射表（脚本/属性面板需要，通常 true）
} GCoreInitDesc;
```

返回：`0` 成功；`-1` 参数非法（`desc` 为空或 `version` 不匹配）。

示例：

```c
GCoreInitDesc desc = {0};
desc.version = (uint32_t)sizeof(GCoreInitDesc);
desc.project_root = "D:/MyGame";
desc.enable_reflection = true;
if (GCore_Init(&desc) != 0) {
    fprintf(stderr, "core init failed\n");
    return 1;
}
```

### 1.2 `GCore_Shutdown`

```c
void GCore_Shutdown(void);
```

作用：销毁 World、场景、脚本运行时、物理世界与音频设备，释放全部实体与资产。必须**先于** `GRender_Shutdown` 调用（渲染器还会读场景数据）。重复调用安全。

示例：

```c
GCore_Shutdown();
GRender_Shutdown();   // 顺序不可颠倒
```

### 1.3 `GCore_IsInitialized`

```c
bool GCore_IsInitialized(void);
```

作用：查询核心是否已初始化。宿主在"可能未初始化"的清理路径上用它做保护。

示例：

```c
if (GCore_IsInitialized()) GCore_Shutdown();
```

### 1.4 `GCore_SetAutoLoadMainScene`

```c
void GCore_SetAutoLoadMainScene(bool enable);
```

作用：控制 `GCore_Init` 完成后是否自动载入 `project.data` 的 `main_scene`（默认 `res:/scenes/main.gesc`）。游戏宿主保持默认开启；需要自己控制场景载入时机的宿主（例如启动到主菜单）关掉它。必须在 `GCore_Init` **之前**调用。

示例：

```c
GCore_SetAutoLoadMainScene(false);   // 启动到主菜单，不进游戏场景
GCore_Init(&desc);
```

### 1.5 `GCore_BeginFrame`

```c
void GCore_BeginFrame(float dt);
```

作用：帧起点。消费命令队列（`GEdit_*` / `GCore_PushCommand` 入队的全部命令）、推进 World（`on_update`、动画、物理、音频）、清零本帧鼠标增量、清空本帧输入事件。`dt` 为秒，建议夹在 `[0.0005, 0.1]` 之间。

示例：

```c
float dt = 1.0f / 60.0f;
GCore_BeginFrame(dt);
```

### 1.6 `GCore_EndFrame`

```c
void GCore_EndFrame(void);
```

作用：帧终点。把新增日志转给日志回调、派发延迟回调（场景载入、实体列表变化、Play 模式切换）、提交撤销快照分组。必须在渲染之后调用。

示例：

```c
GCore_BeginFrame(dt);
script_tick(dt);
GRender_BeginFrame();
GRender_RenderWorld();
GRender_EndFrame();
GCore_EndFrame();
```

### 1.7 `GCore_PushCommand`

```c
int GCore_PushCommand(const GCommand* cmd);
```

作用：把一条原始命令压入命令队列，在下一个 `GCore_BeginFrame` 执行。绝大多数场景不需要直接用它 —— 用 `GEdit_*`（语义封装）或 `GCore_PushCommands`（批量）。

返回：`0` 入队成功；`-1` 队列满或未初始化。

示例：

```c
GCommand cmd = {0};
cmd.type = ECMD_PLAY_MODE;
GCore_PushCommand(&cmd);
```

### 1.8 `GCore_PushCommands`

```c
int GCore_PushCommands(const GCommand* cmds, int count);
```

作用：批量入队，比逐条调用少取 N-1 次锁。用于"一帧内应用一批输入事件"这类高频路径。

返回：被丢弃的条数（`0` 表示全部入队）；参数非法返回 `-1`。

示例：

```c
GCommand batch[8];
for (int i = 0; i < 8; ++i) { memset(&batch[i], 0, sizeof(GCommand)); }
int dropped = GCore_PushCommands(batch, 8);
```

### 1.9 `GCore_GetCmdQueueCapacity`

```c
int GCore_GetCmdQueueCapacity(void);
```

作用：查询命令队列**剩余**可写入条数（不是总容量）。宿主可据此在批量推送前降级（丢事件而不是丢命令）。

示例：

```c
if (GCore_GetCmdQueueCapacity() < 32) {
    // 本帧丢弃低优先级输入事件
}
```

### 1.10 `GCore_GetDroppedCmdCount`

```c
int GCore_GetDroppedCmdCount(void);
```

作用：累计被丢弃的命令数（队列满）。单调递增，用于诊断"界面操作偶发丢失"。

示例：

```c
if (GCore_GetDroppedCmdCount() > 0)
    printf("warning: %d commands dropped so far\n", GCore_GetDroppedCmdCount());
```

### 1.11 `GCore_IsPlaying` / `GCore_IsPaused`

```c
bool GCore_IsPlaying(void);
bool GCore_IsPaused(void);
```

作用：查询 Play 模式与暂停状态。组件 `on_update`、物理、动画只在 `IsPlaying() && !IsPaused()` 时推进。

示例：

```c
const char* state = !GCore_IsPlaying() ? "edit"
                  : (GCore_IsPaused() ? "paused" : "playing");
```

### 1.12 `GCore_SetCallback_UserData`

```c
void GCore_SetCallback_UserData(void* user_data);
```

作用：为下面 9 个回调统一设置 `user_data` 指针（引擎不解释其内容）。只需要设一次。

示例：

```c
GCore_SetCallback_UserData(my_app_state);
```

### 1.13 回调注册（9 个）

```c
void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb);
void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb);
void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb);
void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb);
void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb);
void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb);
void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb);
void GCore_RegisterCallback_OnMouseLock(GOnMouseLock cb);
```

作用：注册事件回调。每个事件只保留**最后一个**注册的函数（传 `NULL` 可注销）。回调在 `GCore_EndFrame` 内统一派发（`OnLogMessage` 除外，日志边产生边转发），此时引擎锁已持有，回调内只能读缓存、不能改场景 —— 把意图记下来，下一帧再执行。

| 回调                    | 触发时机                    | 签名                                                                |
| --------------------- | ----------------------- | ----------------------------------------------------------------- |
| `OnEntitySelected`    | `GEdit_SelectEntity` 生效 | `(GEntityHandle, void*)`                                          |
| `OnEntityDeselected`  | 选择被清空                   | `(void*)`                                                         |
| `OnSceneLoaded`       | 场景载入完成                  | `(const char* path, void*)`                                       |
| `OnPlayModeChanged`   | Play / 暂停 / 停止切换        | `(bool playing, bool paused, void*)`                              |
| `OnEntityListChanged` | 实体增删、改名、改父              | `(void*)`                                                         |
| `OnComponentChanged`  | 组件增删或属性变更               | `(GEntityHandle, uint64_t type_hash, void*)`                      |
| `OnLogMessage`        | 引擎日志（实时）                | `(int level, const char* msg, const char* file, int line, void*)` |
| `OnMouseLock`         | 鼠标锁定状态变化                | `(int locked, void*)`                                             |

示例：

```c
static void on_selected(GEntityHandle e, void* user) {
    AppState* app = (AppState*)user;
    app->selected = e;              // 只记下来，不改场景
}
static void on_log(int level, const char* msg, const char* file, int line, void* user) {
    if (level <= 1) fprintf(stderr, "[%s:%d] %s\n", file, line, msg);
}

GCore_SetCallback_UserData(app);
GCore_RegisterCallback_OnEntitySelected(on_selected);
GCore_RegisterCallback_OnLogMessage(on_log);
```

### 1.14 `GCore_GetLogMessages`

```c
int GCore_GetLogMessages(char* out_buf, int buf_size);
```

作用：把内存日志环（最近若干条，含级别与来源）按 `\n` 拼接拷出，用于自绘控制台或崩溃现场转储。缓冲区不足时截断。

返回：写入字节数；`out_buf` 非法返回 `-1`；日志环不存在返回 `0`。

示例：

```c
char buf[4096];
int n = GCore_GetLogMessages(buf, (int)sizeof(buf));
if (n > 0) fwrite(buf, 1, (size_t)n, stderr);
```

### 1.15 `GCore_Undo` / `GCore_Redo`

```c
int GCore_Undo(void);
int GCore_Redo(void);
```

作用：快照式撤销/重做。执行前先收口未结束的编辑分组（`GCore_EndEdit`），所以拖拽中途按撤销不会把半个拖拽当成一步。Play 模式下撤销栈冻结。

返回：`1` 执行了；`0` 无可撤销/重做。

示例：

```c
if (input_ctrl_z_pressed()) GCore_Undo();
if (input_ctrl_y_pressed()) GCore_Redo();
```

### 1.16 `GCore_GetUndoCount` / `GCore_GetRedoCount`

```c
int GCore_GetUndoCount(void);
int GCore_GetRedoCount(void);
```

作用：查询撤销/重做栈深度，用于置灰菜单项。

示例：

```c
menu_item("Undo", "Ctrl+Z", GCore_GetUndoCount() > 0);
menu_item("Redo", "Ctrl+Y", GCore_GetRedoCount() > 0);
```

### 1.17 `GCore_ClearUndoHistory`

```c
void GCore_ClearUndoHistory(void);
```

作用：清空两个栈。载入新场景、切换 2D/3D 槽之后调用，避免把上一个场景的快照恢复到新场景上。

示例：

```c
GScene_Load("res:/scenes/level2.gesc");
GCore_ClearUndoHistory();
```

### 1.18 `GCore_SetUndoLimit`

```c
void GCore_SetUndoLimit(int limit);
```

作用：设置撤销栈深度上限（默认 64）。`0` 表示完全关闭记录（降低内存占用，适合纯运行时游戏构建）。

示例：

```c
GCore_SetUndoLimit(0);   // 游戏运行时不需要撤销
```

### 1.19 `GCore_BeginEdit` / `GCore_EndEdit`

```c
void GCore_BeginEdit(const char* label);
void GCore_EndEdit(void);
```

作用：把一段连续编辑合并成一步撤销。`BeginEdit` 抓快照，期间所有变更命令都不再单独入栈，`EndEdit` 提交为一步。滑块拖拽、Gizmo 拖动、批量属性修改都必须包起来。

示例：

```c
if (drag_started) GCore_BeginEdit("Move Entity");
if (dragging)     GEdit_TranslateEntity(entity, &delta);
if (drag_ended)   GCore_EndEdit();
```

### 1.20 `GCore_TakeRenderTargetRequest`

```c
int GCore_TakeRenderTargetRequest(int target, int* out_w, int* out_h);
```

作用：取出"离屏目标改尺寸"请求。`target` 为 `0`（场景视口）或 `1`（游戏视口）。core 不能反向调用渲染器，只能由渲染侧在帧起点轮询；取走后即清除，因此每帧最多取到一次。

返回：`1` 取到新尺寸；`0` 没有待处理请求。

示例：

```c
int w = 0, h = 0;
if (GCore_TakeRenderTargetRequest(0, &w, &h))
    GViewport_SetSize(w, h);       // 渲染侧接口
```

### 1.21 资源打包（4 个）

```c
GPackHandle GCore_PackCreate(void);
int         GCore_PackAddFile(GPackHandle handle, const char* internal_path, const char* source_path);
int         GCore_PackWrite(GPackHandle handle, const char* output_path);
void        GCore_PackDestroy(GPackHandle handle);
```

作用：写 `.gpkg` / `.gpack` 资源包（格式与运行期 `GPackReader` 共用）。主要给 GryceGC 打包工具用。

返回：`PackCreate` 返回句柄（失败 `NULL`）；`PackAddFile` / `PackWrite` 返回 `0` 成功、`-1` 失败。

示例：

```c
GPackHandle pack = GCore_PackCreate();
GCore_PackAddFile(pack, "scenes/main.gesc", "D:/MyGame/scenes/main.gesc");
GCore_PackAddFile(pack, "models/cube.obj",  "D:/MyGame/models/cube.obj");
GCore_PackWrite(pack, "D:/MyGame/game.gpkg");
GCore_PackDestroy(pack);
```


### 1.22 `GCore_GetInternalWorldPtr`

```c
void* GCore_GetInternalWorldPtr(void);
```

作用：取内部 `World*`（不透明指针），供同进程其它引擎 DLL（`GryceRenderer`）内部转换使用。**不属于公共 API**：内容随版本变化，宿主不要解引用。

示例：

```c
void* world = GCore_GetInternalWorldPtr();   // 仅透传给引擎内部模块
```

---

## 2. 实体（entity_api.h）

### 2.1 `GEntity_GetCount`

```c
int GEntity_GetCount(void);
```

作用：当前场景里参与层级树的实体数量（无父节点的实体不计入，见下方注意）。

注意：该计数口径是"有父节点的实体"，空场景与只有根实体的场景都返回 0。需要"场景里到底有哪些实体"时用 `GEntity_GetAt` 逐个枚举并配合 `GEntity_GetPath`，不要用计数做循环边界以外的断言。

示例：

```c
for (int i = 0; i < GEntity_GetCount(); ++i) {
    GEntityHandle e = GEntity_GetAt(i);
    char path[256];
    if (GEntity_GetPath(e, path, sizeof(path)) >= 0) printf("%s\n", path);
}
```

### 2.2 `GEntity_GetAt`

```c
GEntityHandle GEntity_GetAt(int index);
```

作用：按枚举下标取实体句柄。下标越界返回 0。枚举顺序是场景内部的稳定顺序，不是层级遍历顺序。

示例：

```c
GEntityHandle first = GEntity_GetAt(0);
if (first == 0) printf("empty scene\n");
```

### 2.3 `GEntity_GetName`

```c
int GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size);
```

作用：取实体名（不含层级路径）。

返回：写入字符数；句柄无效或缓冲非法返回 `-1`。

示例：

```c
char name[128];
if (GEntity_GetName(e, name, sizeof(name)) >= 0) printf("name=%s\n", name);
```

### 2.4 `GEntity_GetPath`

```c
int GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size);
```

作用：取从根到该实体的层级路径（`Root/Player/Camera` 形式），用于日志、调试与"同名实体区分"。

示例：

```c
char path[512];
GEntity_GetPath(e, path, sizeof(path));
GLOG_INFO("selected: {}", path);
```

### 2.5 `GEntity_GetParent`

```c
GEntityHandle GEntity_GetParent(GEntityHandle entity);
```

作用：取父实体句柄；已是根或句柄无效返回 0。配合 `GEntity_GetChildCount` / `GEntity_GetChildAt` 做层级遍历。

示例：

```c
for (GEntityHandle e = GEntity_GetParent(child); e != 0; e = GEntity_GetParent(e))
    printf("ancestor: %d\n", e);
```

### 2.6 `GEntity_GetChildCount`

```c
int GEntity_GetChildCount(GEntityHandle entity);
```

作用：直接子实体数量（不含孙辈）。句柄无效返回 0。

示例：

```c
int n = GEntity_GetChildCount(root);
```

### 2.7 `GEntity_GetChildAt`

```c
GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index);
```

作用：取第 `index` 个直接子实体；越界或无效返回 0。子项顺序即场景文件里的挂载顺序。

示例：

```c
for (int i = 0, n = GEntity_GetChildCount(root); i < n; ++i) {
    GEntityHandle child = GEntity_GetChildAt(root, i);
    printf("child %d = %d\n", i, child);
}
```

### 2.8 `GEntity_GetSiblingIndex`

```c
int GEntity_GetSiblingIndex(GEntityHandle entity);
```

作用：取实体在父级子列表中的序号（0 起），用于上移/下移、拖放重排。根实体或无效返回 `-1`。

示例：

```c
int idx = GEntity_GetSiblingIndex(e);
printf("index among siblings: %d\n", idx);
```

### 2.9 `GEntity_GetSelected`

```c
GEntityHandle GEntity_GetSelected(void);
```

作用：取当前选中实体（由 `GEdit_SelectEntity` 设置）。未选中返回 0。

示例：

```c
GEntityHandle sel = GEntity_GetSelected();
if (sel) printf("selected %d\n", sel);
```

### 2.10 局部变换读取（3 个）

```c
int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos);
int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot);
int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale);
```

作用：读实体相对父级的局部 TRS。返回 `0` 成功、`-1` 失败。世界变换由父链累乘，需要世界值时用 2.12 的三个接口，内部从世界矩阵分解 TRS。

示例：

```c
GVec3 p; GQuat q; GVec3 s;
GEntity_GetLocalPosition(e, &p);
GEntity_GetLocalRotation(e, &q);
GEntity_GetLocalScale(e, &s);
printf("local pos=(%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
```

### 2.11 局部变换写入（3 个）

```c
int GEntity_SetLocalPosition(GEntityHandle entity, const GVec3* pos);
int GEntity_SetLocalRotation(GEntityHandle entity, const GQuat* rot);
int GEntity_SetLocalScale(GEntityHandle entity, const GVec3* scale);
```

作用：**直接**写实体变换，立即生效、**不入撤销栈、不走命令队列**。适用于运行期逻辑（相机跟随、程序化运动）；编辑器式的用户操作必须用 `GEdit_SetTransform` / `GEdit_TranslateEntity`，否则撤销会对不上。

示例：

```c
GVec3 p = {0.0f, 1.5f, 0.0f};
GEntity_SetLocalPosition(e, &p);
```

### 2.12 `GEntity_GetWorldPosition` / `GetWorldRotation` / `GetWorldScale`

```c
int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos);
int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot);
int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale);
```

作用：读父链累乘后的世界 TRS。返回 `0` 成功、`-1` 失败。

示例：

```c
GVec3 wp;
GEntity_GetWorldPosition(e, &wp);
printf("world y = %.2f\n", wp.y);
```

### 2.13 `GEntity_ExportJson`

```c
int GEntity_ExportJson(GEntityHandle entity, char* out_buf, int buf_size);
```

作用：把实体及其全部子孙导出为 JSON（扁平 `entities` 数组，根的 `parent` 为 `null`），用于复制粘贴、Prefab 创建、自定义存档。

返回：写入字节数；缓冲区不足返回 `-1`（与其它字符串接口不同：这里不截断，因为截断的 JSON 无法解析）。

示例：

```c
char json[64 * 1024];
int n = GEntity_ExportJson(e, json, sizeof(json));
if (n < 0) { /* 缓冲区不够：换更大的缓冲重试 */ }
```

### 2.14 `GEntity_ImportJson`

```c
GEntityHandle GEntity_ImportJson(const char* json, GEntityHandle parent_handle);
```

作用：把 `GEntity_ExportJson` 产出的 JSON 导入为子树，挂到 `parent_handle` 下（0 = 场景根）。导入时生成全新的 UUID / EntityID，因此同一份 JSON 可以粘贴多次。

返回：新子树根句柄；解析失败返回 0。

示例：

```c
GEntityHandle copy = GEntity_ImportJson(json, GEntity_GetParent(e));
```

### 2.15 `GEntity_SaveAsPrefab`

```c
int GEntity_SaveAsPrefab(GEntityHandle entity, const char* path);
```

作用：把实体子树存为 Prefab 文件（`.gesc` / `.geprefab`），根的父引用被截断，实例化时重新挂载。返回 `0` 成功、`-1` 失败。

示例：

```c
GEntity_SaveAsPrefab(e, "res:/prefabs/enemy.geprefab");
```

### 2.16 `GEntity_CreatePrefabInstance`

```c
GEntityHandle GEntity_CreatePrefabInstance(const char* prefab_path, GEntityHandle parent_handle);
```

作用：实例化 Prefab 到场景并自动挂 `PrefabInstance` 组件（记录模板路径），用于后续 Apply / Revert。返回实例根句柄，失败 0。

示例：

```c
GEntityHandle enemy = GEntity_CreatePrefabInstance("res:/prefabs/enemy.geprefab", root);
```

### 2.17 `GEntity_ApplyPrefab`

```c
int GEntity_ApplyPrefab(GEntityHandle entity);
```

作用：把实例当前状态写回模板文件（Apply），其它实例下次载入时获得这次修改。返回 `0` 成功。

示例：

```c
GEntity_ApplyPrefab(enemy);
```

### 2.18 `GEntity_RevertPrefab`

```c
int GEntity_RevertPrefab(GEntityHandle entity);
```

作用：把实例还原为"模板 + 覆盖参数"的状态（Revert），丢弃本地修改。返回 `0` 成功。

示例：

```c
GEntity_RevertPrefab(enemy);
```

---

## 3. 组件（component_api.h）

### 3.0 三个必须先记住的口径

1. **下标空间**：本模块所有 `index` / `child_index` / `occurrence` 都指向"实体的可见组件序列"，已过滤掉 `Transform`、`ParentComponent`、`ChildrenComponent` 三个内部组件。界面、脚本、存档三方必须用同一口径，否则会错位。
2. **类型 hash**：一律是「可读全名的 `std::hash<std::string>`」，例如 `"gryce_engine::components::d2::basic_rect::ColorRect"`。下面所有 `uint64_t comp_type_hash` 都吃这一口径，且支持基类查询（传基类 hash 能命中派生实例）。
3. **两轴正交**：类型继承（基类/子类）与实例挂载（父组件/子组件）是两回事，各自有一套接口。

### 3.1 `GComponent_GetCount`

```c
int GComponent_GetCount(GEntityHandle entity);
```

作用：实体上可见组件的数量。实体无效返回 0。

示例：

```c
printf("components: %d\n", GComponent_GetCount(e));
```

### 3.2 `GComponent_GetTypeHashAt`

```c
int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash);
```

作用：取第 `index` 个组件的类型全名 hash。返回 `0` 成功、`-1` 失败。拿到的 hash 可直接喂给本模块其它接口。

示例：

```c
uint64_t h = 0;
if (GComponent_GetTypeHashAt(e, 0, &h) == 0) printf("type hash = %llu\n", (unsigned long long)h);
```

### 3.3 `GComponent_GetTypeNameAt`

```c
int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size);
```

作用：取第 `index` 个组件的可读全名（与 3.18 的 `GetTypeNameByHash` 同形态）。返回写入长度，失败 `-1`。

示例：

```c
char tn[256];
GComponent_GetTypeNameAt(e, 1, tn, sizeof(tn));
printf("component 1 is %s\n", tn);
```

### 3.4 `GComponent_GetPropertyCount`

```c
int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash);
```

作用：该类型暴露的反射字段数量。返回 `-1` 表示实体或类型不存在。属性面板据此生成行，不需要在界面上写死字段名。

示例：

```c
int n = GComponent_GetPropertyCount(e, mesh_renderer_hash);
for (int i = 0; i < n; ++i) { /* 见 3.5 */ }
```

### 3.5 `GComponent_GetPropertyInfo`

```c
int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,
                               char* out_name, int name_buf_size,
                               int* out_type, int* out_size);
```

作用：取第 `prop_index` 个字段的名字、类型编号与字节宽度。返回 `0` 成功、`-1` 失败。`out_type` 的取值与反射层 `PropertyType` 一致（float / int / bool / vec2/3/4 / color / string 等）。

示例：

```c
char name[64]; int type = 0, size = 0;
GComponent_GetPropertyInfo(e, h, 0, name, sizeof(name), &type, &size);
printf("field %s type=%d size=%d\n", name, type, size);
```

### 3.6 `GComponent_GetProperty`

```c
int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           void* out_value, int value_size);
```

作用：按字段名读原始字节。标量/向量按 `value_size` 拷贝；字符串写入 `char[]`（保证 `\0`）。返回 `0` 成功、`-1` 失败（字段不存在或缓冲不足）。

示例：

```c
float roughness = 0.0f;
GComponent_GetProperty(e, mesh_hash, "roughness", &roughness, sizeof(roughness));
```

### 3.7 `GComponent_SetProperty`

```c
int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           const void* value, int value_size);
```

作用：按字段名写原始字节，**立即生效**（不进撤销栈）。用户操作请走 `GEdit_SetProperty`。返回 `0` 成功、`-1` 失败。

示例：

```c
float metallic = 1.0f;
GComponent_SetProperty(e, mesh_hash, "metallic", &metallic, sizeof(metallic));
```

### 3.8 Tilemap 瓦片（2 个）

```c
int GComponent_TilemapGetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                               int* out_tiles, int max_count);
int GComponent_TilemapSetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                               const int* tiles, int count);
```

作用：读写 Tilemap 的瓦片数组（`std::vector<int>` 不在反射标量范围内，只能走专用接口）。`GetTiles` 返回实际写入数量（失败 `-1`）；`SetTiles` 的 `count = -1` 表示清空，返回 `0` 成功。

示例：

```c
int tiles[64];
int n = GComponent_TilemapGetTiles(e, tilemap_hash, tiles, 64);
tiles[0] = 3;
GComponent_TilemapSetTiles(e, tilemap_hash, tiles, n);
```

### 3.9 `GComponent_AddComponent` / `GComponent_RemoveComponent`

```c
int GComponent_AddComponent(GEntityHandle entity, uint64_t comp_type_hash);
int GComponent_RemoveComponent(GEntityHandle entity, uint64_t comp_type_hash);
```

作用：加/删组件。两者都**走命令队列**（下一帧生效、进撤销栈），返回 `0` 表示已入队、`-1` 表示类型未知或队列满。

示例：

```c
uint64_t h = 0;
GComponent_GetTypeHashByName("ColorRect", &h);
GComponent_AddComponent(e, h);
GComponent_RemoveComponent(e, h);
```

### 3.10 `GComponent_MeshSetMaterial`

```c
int GComponent_MeshSetMaterial(GEntityHandle entity, const char* mesh_path,
                               float r, float g, float b,
                               float roughness, float metallic);
```

作用：给实体（无则自动加）`MeshRenderer` 并设置网格路径与 PBR 参数。`mesh_path` 传 `NULL`/空串表示只改颜色（要求已存在 `MeshRenderer`）。直接改嵌套的 `material` 对象，因为反射标量触达不到它。

示例：

```c
GComponent_MeshSetMaterial(e, "res:/models/sphere.obj", 1.0f, 0.2f, 0.2f, 0.3f, 0.0f);
```


### 3.11 `GComponent_MeshSetMaterialPreset`

```c
int GComponent_MeshSetMaterialPreset(GEntityHandle entity, const char* preset_name);
```

作用：按预设名设置 PBR 材质（预设表见 `render/material_presets.h`，如 `metal` / `plastic` / `glass`）。预设不存在返回 `-1` 且材质保持原状；缺 `MeshRenderer` 时自动创建。

示例：

```c
GComponent_MeshSetMaterialPreset(e, "metal");
```

### 3.12 `GComponent_MeshGetPath`

```c
int GComponent_MeshGetPath(GEntityHandle entity, char* out_buf, int buf_size);
```

作用：读 `MeshRenderer` 的网格路径。返回 `0` 成功、`-1` 失败（无实体 / 无组件 / 路径为空）。

示例：

```c
char mesh[512];
if (GComponent_MeshGetPath(e, mesh, sizeof(mesh)) == 0) printf("mesh=%s\n", mesh);
```

### 3.13 `GComponent_MeshGetMaterial`

```c
int GComponent_MeshGetMaterial(GEntityHandle entity,
                               float* r, float* g, float* b,
                               float* roughness, float* metallic);
```

作用：读 `MeshRenderer` 嵌套材质的 PBR 参数。返回 `0` 成功、`-1` 失败。

示例：

```c
float r, g, b, rough, metal;
GComponent_MeshGetMaterial(e, &r, &g, &b, &rough, &metal);
```

### 3.14 `GComponent_GetRegisteredTypeCount`

```c
int GComponent_GetRegisteredTypeCount(void);
```

作用：注册到工厂的组件类型总数（含内部类型）。用于生成"可添加组件"列表。

示例：

```c
for (int i = 0, n = GComponent_GetRegisteredTypeCount(); i < n; ++i) {
    uint64_t h = 0; char name[128];
    if (GComponent_GetRegisteredTypeInfo(i, &h, name, sizeof(name)) == 0)
        printf("%s\n", name);
}
```

### 3.15 `GComponent_GetRegisteredTypeInfo`

```c
int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size);
```

作用：取第 `index` 个注册类型的 hash 与短名。返回 `0` 成功、`-1` 越界。

注意：工厂注册但反射表里没有的类型（`ParentComponent` / `ChildrenComponent` / `SubViewport`）回退为「短名 hash」，与全名 hash 口径不同；对外做属性编辑时应过滤掉它们。

示例：

```c
uint64_t h; char name[128];
GComponent_GetRegisteredTypeInfo(3, &h, name, sizeof(name));
```

### 3.16 `GComponent_GetRegisteredTypeCategory`

```c
int GComponent_GetRegisteredTypeCategory(int index, char* out_category, int category_buf_size);
```

作用：取类型分类（`2d` / `3d` / `core` / `physics` 等），用于在"添加组件"菜单里分组。返回 `0` 成功。

示例：

```c
char cat[32];
GComponent_GetRegisteredTypeCategory(i, cat, sizeof(cat));
```

### 3.17 `GComponent_GetTypeHashByName`

```c
int GComponent_GetTypeHashByName(const char* type_full_name, uint64_t* out_hash);
```

作用：按名字取类型 hash，接受三种写法：反射表短名（`ColorRect`）、工厂注册名（`Script`）、可读全名（`gryce_engine::...::ColorRect`）。这是外部代码获取 hash 的推荐入口。

示例：

```c
uint64_t h = 0;
GComponent_GetTypeHashByName("MeshRenderer", &h);
```

### 3.18 `GComponent_GetTypeNameByHash`

```c
int GComponent_GetTypeNameByHash(uint64_t type_hash, char* out_buf, int buf_size);
```

作用：全名 hash → 可读全名。返回写入字符数；反射表里没有的 hash 返回 `-1`。

示例：

```c
char full[256];
GComponent_GetTypeNameByHash(h, full, sizeof(full));
```

### 3.19 `GComponent_GetParentAt`

```c
int GComponent_GetParentAt(GEntityHandle entity, int index);
```

作用：取第 `index` 个组件的父组件下标（同一可见下标空间）。没有父组件或下标非法返回 `-1`。

示例：

```c
int parent = GComponent_GetParentAt(e, 2);
if (parent >= 0) printf("attached to component %d\n", parent);
```

### 3.20 `GComponent_SetParentAt`

```c
int GComponent_SetParentAt(GEntityHandle entity, int index, int parent_index);
```

作用：把第 `index` 个组件挂到第 `parent_index` 个组件下；`parent_index = -1` 表示变成组件树根。会拒绝"挂到自己下面"与"成环"。返回 `1` 成功、`0` 失败。

示例：

```c
GComponent_SetParentAt(e, 3, 1);   // 把组件 3 挂到组件 1 下
GComponent_SetParentAt(e, 3, -1);  // 又摘回根
```

### 3.21 `GComponent_GetChildCountAt`

```c
int GComponent_GetChildCountAt(GEntityHandle entity, int index);
```

作用：第 `index` 个组件的直接子组件数量。下标非法返回 `-1`。

示例：

```c
int kids = GComponent_GetChildCountAt(e, 1);
```

### 3.22 `GComponent_GetChildAt`

```c
int GComponent_GetChildAt(GEntityHandle entity, int index, int child_index);
```

作用：取第 `index` 个组件的第 `child_index` 个子组件的**组件下标**（不是句柄）。下标非法返回 `-1`。

示例：

```c
for (int i = 0, n = GComponent_GetChildCountAt(e, 1); i < n; ++i) {
    int ci = GComponent_GetChildAt(e, 1, i);
    printf("child component index = %d\n", ci);
}
```

### 3.23 `GComponent_IsEffectivelyEnabledAt`

```c
int GComponent_IsEffectivelyEnabledAt(GEntityHandle entity, int index);
```

作用：生命周期继承判定 —— 自身 `enabled` 且整条父链都 enabled 才返回 1。下标非法返回 0。

示例：

```c
if (!GComponent_IsEffectivelyEnabledAt(e, idx)) continue;   // 跳过被父级关掉的组件
```

### 3.24 `GComponent_GetLocalTransformAt`

```c
int GComponent_GetLocalTransformAt(GEntityHandle entity, int index,
                                   float* out_pos3, float* out_quat4, float* out_scale3);
```

作用：取组件相对父组件的局部变换（根组件相对宿主实体）。三个输出数组分别是 `[x,y,z]`、`[x,y,z,w]`、`[x,y,z]`。返回 `0` 成功、`-1` 失败。

示例：

```c
float p3[3], q4[4], s3[3];
GComponent_GetLocalTransformAt(e, idx, p3, q4, s3);
```

### 3.25 `GComponent_SetLocalTransformAt`

```c
int GComponent_SetLocalTransformAt(GEntityHandle entity, int index,
                                   const float* pos3, const float* quat4, const float* scale3);
```

作用：写组件的局部变换（组件树语义）。返回 `1` 成功、`0` 失败。编辑器式操作走 `GCmdSetComponentLocalTransform` 才有撤销。

示例：

```c
float p3[3] = {0.0f, 1.0f, 0.0f};
float q4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
float s3[3] = {1.0f, 1.0f, 1.0f};
GComponent_SetLocalTransformAt(e, idx, p3, q4, s3);
```

### 3.26 `GComponent_IsDerivedFromAt`

```c
int GComponent_IsDerivedFromAt(GEntityHandle entity, int index, uint64_t base_type_hash);
```

作用：判断第 `index` 个组件的类型是否派生自 `base_type_hash`（基类查询）。返回 `1` 派生 / `0` 不是（含下标非法）。这是"按基类批量处理（例如给所有 `BasicRect` 设颜色）"的关键接口。

示例：

```c
uint64_t basic_rect = 0;
GComponent_GetTypeHashByName("BasicRect", &basic_rect);
if (GComponent_IsDerivedFromAt(e, idx, basic_rect)) { /* ColorRect 也会命中 */ }
```

### 3.27 `GComponent_GetTypeChainAt`

```c
int GComponent_GetTypeChainAt(GEntityHandle entity, int index,
                              uint64_t* out_hashes, int max_count);
```

作用：取组件的类型祖先链（根 → 派生，含自身），每项是全名 hash。

返回：实际写入的个数；`out_hashes` 传 `NULL` 时只返回链长（供调用方先分配缓冲）；下标非法返回 `-1`。

示例：

```c
int len = GComponent_GetTypeChainAt(e, idx, NULL, 0);
uint64_t chain[8];
GComponent_GetTypeChainAt(e, idx, chain, len < 8 ? len : 8);
```

---

## 4. 场景（scene_api.h）

### 4.1 `GScene_Load`

```c
int GScene_Load(const char* path);
```

作用：同步载入 `.gesc` 场景，替换当前场景。旧场景的实体句柄全部失效。载入完成后触发 `OnSceneLoaded`。

返回：`0` 成功、`-1` 失败（未初始化 / 路径为空 / 解析失败）。

示例：

```c
if (GScene_Load("res:/scenes/main.gesc") != 0)
    GLOG_ERROR("load scene failed");
```

### 4.2 `GScene_Save`

```c
int GScene_Save(const char* path);
```

作用：把当前场景写盘，并把它记为"当前路径"（影响 `GScene_GetCurrentPath` 与 2D/3D 槽记录）。返回 `0` 成功、`-1` 失败。

示例：

```c
GScene_Save("res:/scenes/main.gesc");
```

### 4.3 `GScene_GetCurrentPath`

```c
int GScene_GetCurrentPath(char* out_buf, int buf_size);
```

作用：取当前场景路径（可能是 `res:/` 形式）。未载入时写入空串。返回写入长度。

示例：

```c
char p[512];
GScene_GetCurrentPath(p, sizeof(p));
printf("current scene: %s\n", p);
```

### 4.4 `GScene_New`

```c
int GScene_New(void);
```

作用：新建空场景替换当前场景；默认路径按当前槽决定（`res:/scenes/scene_2d.gesc` 或 `scene_3d.gesc`）。返回 `0` 成功。

示例：

```c
GScene_New();
GCore_ClearUndoHistory();
```

### 4.5 `GScene_PickScreen`

```c
GEntityHandle GScene_PickScreen(float sx, float sy,
                                int viewport_w, int viewport_h,
                                GEntityHandle camera_entity);
```

作用：屏幕坐标拾取。从 `camera_entity` 的 `Transform` + `Camera` 组件发射射线，与带 `MeshRenderer` / `SkinnedMeshRenderer` 的实体做**世界 AABB** 求交，返回最近命中实体。`sx/sy` 为视口内像素坐标（左上原点）。

返回：命中实体；未命中返回 0。

示例：

```c
GEntityHandle hit = GScene_PickScreen(mouse_x - vp_x, mouse_y - vp_y, vp_w, vp_h, camera);
```

### 4.6 `GScene_PickRay`

```c
GEntityHandle GScene_PickRay(const GVec3* origin, const GVec3* direction, float max_dist);
```

作用：世界空间射线拾取（`direction` 无需归一化）。`max_dist <= 0` 表示不限距离。返回最近命中实体，未命中 0。

示例：

```c
GVec3 o = {0.0f, 2.0f, 0.0f}, d = {0.0f, -1.0f, 0.0f};
GEntityHandle ground = GScene_PickRay(&o, &d, 100.0f);
```

### 4.7 `GScene_GetMode`

```c
int GScene_GetMode(void);
```

作用：当前活动场景槽：`0` = 2D，`1` = 3D。

示例：

```c
printf("active slot = %s\n", GScene_GetMode() == 0 ? "2D" : "3D");
```

### 4.8 `GScene_SetMode`

```c
int GScene_SetMode(int mode);
```

作用：热切换场景槽。当前场景存入旧槽（内存保留），载入目标槽场景；目标槽为空则新建空场景。返回 `0` 成功。

示例：

```c
GScene_SetMode(1);   // 切到 3D 槽
```

### 4.9 `GScene_ReleaseMode`

```c
int GScene_ReleaseMode(int mode);
```

作用：释放指定槽的场景内存与路径记录；若释放的是活动槽，替换为空场景。返回 `0` 成功。

示例：

```c
GScene_ReleaseMode(0);   // 关掉 2D 槽，省内存
```

### 4.10 `GScene_HasScene`

```c
bool GScene_HasScene(int mode);
```

作用：指定槽是否已有场景。用于决定菜单项是否可用。

示例：

```c
if (GScene_HasScene(0)) menu_item("Switch to 2D", "", true);
```

---

## 5. 资产（asset_api.h）

### 5.1 `GAsset_Import`

```c
GAssetHandle GAsset_Import(const char* source_path);
```

作用：显式导入资产：注册句柄、解析模型/贴图、并按需生成侧车 `.meta`（只有显式导入才写盘，普通加载不写）。返回资产句柄，失败 `<= 0`。

示例：

```c
GAssetHandle a = GAsset_Import("res:/models/character.fbx");
if (a <= 0) GLOG_ERROR("import failed");
```

### 5.2 `GAsset_Load`

```c
GAssetHandle GAsset_Load(const char* path);
```

作用：加载并注册资产，但不写侧车。适合运行期只读加载。返回句柄，失败 `<= 0`。

示例：

```c
GAssetHandle tex = GAsset_Load("res:/textures/brick.png");
```

### 5.3 `GAsset_GetPath`

```c
int GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size);
```

作用：由句柄反查资产路径。返回 `0` 成功、`-1` 句柄无效。

示例：

```c
char p[512];
GAsset_GetPath(a, p, sizeof(p));
```

### 5.4 `GAsset_Unload`

```c
void GAsset_Unload(GAssetHandle handle);
```

作用：注销句柄并释放引用（引用计数归零时卸载数据）。句柄无效时无操作。

示例：

```c
GAsset_Unload(a);
```

### 5.5 `GAsset_ReadMeta`

```c
int GAsset_ReadMeta(const char* asset_path, GAssetMetaInfo* out);
```

作用：读取 `<asset>.meta` 侧车（guid、导入设置）。不存在或损坏返回 `0` 且不改动 `out`。

示例：

```c
GAssetMetaInfo info;
if (GAsset_ReadMeta("res:/models/cube.obj", &info))
    printf("guid=%s scale=%.3f\n", info.guid, info.scale);
```

### 5.6 `GAsset_EnsureMeta`

```c
int GAsset_EnsureMeta(const char* asset_path, GAssetMetaInfo* out);
```

作用：读侧车；不存在则按源文件生成一份（含新 guid 与文件指纹）并写盘。目录只读时只返回内存结果。返回 `1` 存在/已创建、`0` 失败。

示例：

```c
GAssetMetaInfo info;
if (!GAsset_EnsureMeta(path, &info)) GLOG_WARN("meta unavailable: {}", path);
```

### 5.7 `GAsset_WriteMeta`

```c
int GAsset_WriteMeta(const char* asset_path, const GAssetMetaInfo* info);
```

作用：写侧车；缺失字段用引擎默认值补齐。返回 `1`/`0`。这是"改导入设置"的唯一入口。

示例：

```c
GAssetMetaInfo info;
GAsset_EnsureMeta(path, &info);
info.scale = 0.01f;      // 源单位 cm -> m
info.up_axis = 2;        // Z-up
GAsset_WriteMeta(path, &info);
```

### 5.8 `GAsset_IsMetaStale`

```c
int GAsset_IsMetaStale(const char* asset_path);
```


作用：源文件指纹与侧车记录不一致时返回 1，界面据此提示"资源已变更，是否重新导入"。

示例：

```c
if (GAsset_IsMetaStale(path)) show_reimport_banner = 1;
```

### 5.9 `GAsset_ScanDirectory`

```c
int GAsset_ScanDirectory(const char* dir, int recursive, char* out_buf, int buf_size);
```

作用：遍历目录并输出单行 JSON 清单（`path/name/ext/size/guid/has_meta/stale/kind`），按 path 字典序，保证同一目录每次顺序一致。`dir` 支持 `res:/` 与真实路径；只做文件系统遍历，不加载数据。

返回：写入字节数；缓冲不足时返回**所需**长度（大于 `buf_size`，调用方据此扩容重试）；失败 `-1`。

示例：

```c
char buf[8192];
int n = GAsset_ScanDirectory("res:/models", 1, buf, sizeof(buf));
if (n > (int)sizeof(buf)) {
    char* big = (char*)malloc((size_t)n + 1);
    GAsset_ScanDirectory("res:/models", 1, big, n + 1);
    free(big);
}
```

### 5.10 `GAsset_ScanDirectoryEach`

```c
typedef int (*GAssetVisitor)(void* user, const GAssetEntry* entry);
int GAsset_ScanDirectoryEach(const char* dir, int recursive, GAssetVisitor visitor, void* user);
```

作用：访客式遍历。与 5.9 同样的目录、顺序与排除规则，但直接回调，不经过 JSON —— 界面自己画列表时用它，避免"生成 JSON 再解析"的双重开销与"解析失败显示空白"的排查黑洞。回调里拿到的指针只在本次调用期间有效；返回 0 可中止遍历。

返回：访问到的条目数；目录不存在返回 `-1`。

示例：

```c
static int on_entry(void* user, const GAssetEntry* e) {
    AssetList* list = (AssetList*)user;
    if (e->has_meta && !e->stale) list_push(list, e->path, e->kind);
    return 1;                      // 0 = 中止
}
GAsset_ScanDirectoryEach("res:/models", 1, on_entry, &list);
```

---

## 6. 材质（material_api.h）

材质字段用 `GMaterialField` 枚举定位，`comp_type_hash` 指 `MeshRenderer` 或 `SkinnedMeshRenderer` 的全名 hash。

### 6.1 `GMaterial_GetField`

```c
int GMaterial_GetField(GEntityHandle entity, uint64_t comp_type_hash,
                       int field, float* out_floats, int float_capacity,
                       char* out_str, int str_capacity);
```

作用：读一个材质字段。标量/向量写 `out_floats`（容量 `float_capacity`），字符串写 `out_str`。返回 `0` 成功、`-1` 失败（实体/组件/字段无效）。

示例：

```c
float rgb[3];
GMaterial_GetField(e, mesh_hash, GMAT_ALBEDO_COLOR, rgb, 3, NULL, 0);
char path[512];
GMaterial_GetField(e, mesh_hash, GMAT_ALBEDO_MAP_PATH, NULL, 0, path, sizeof(path));
```

### 6.2 `GMaterial_SetField`

```c
int GMaterial_SetField(GEntityHandle entity, uint64_t comp_type_hash,
                       int field, const float* in_floats, int float_count,
                       const char* in_str);
```

作用：写一个材质字段。字符串字段传 `in_str`，其余传 `in_floats`。改贴图路径或 `GMAT_USE_*_MAP` 会把材质标记为 `textures_dirty`，渲染线程下一帧自动重新上传贴图。返回 `0` 成功。

示例：

```c
float rough = 0.25f;
GMaterial_SetField(e, mesh_hash, GMAT_ROUGHNESS, &rough, 1, NULL);
GMaterial_SetField(e, mesh_hash, GMAT_ALBEDO_MAP_PATH, NULL, 0, "res:/textures/metal.png");
float on = 1.0f;
GMaterial_SetField(e, mesh_hash, GMAT_USE_ALBEDO_MAP, &on, 1, NULL);
```

### 6.3 `GMaterial_LoadFromFile`

```c
int GMaterial_LoadFromFile(GEntityHandle entity, uint64_t comp_type_hash, const char* path);
```

作用：把 `.gmat` 文件整套载入到实体的渲染组件。返回 `0` 成功、`-1` 失败（组件无效或文件不存在）。

示例：

```c
GMaterial_LoadFromFile(e, mesh_hash, "res:/materials/steel.gmat");
```

---

## 7. 动画（animator_api.h）

全部接口都需要 `SkinnedMeshRenderer` 的全名 hash。片段 / 骨骼 / 轨道 / 关键帧都是只读查询，用于自建动画预览、时间轴、状态机。

### 7.1 `GAnimator_GetClipCount`

```c
int GAnimator_GetClipCount(GEntityHandle entity, uint64_t comp_type_hash);
```

作用：模型自带动画片段数量。实体/组件无效返回 `-1`。

示例：

```c
int clips = GAnimator_GetClipCount(e, skinned_hash);
```

### 7.2 `GAnimator_GetClipName`

```c
int GAnimator_GetClipName(GEntityHandle entity, uint64_t comp_type_hash,
                          int index, char* out_buf, int buf_size);
```

作用：取第 `index` 个片段名。返回写入长度，失败 `-1`。

示例：

```c
char name[128];
GAnimator_GetClipName(e, skinned_hash, 0, name, sizeof(name));
```

### 7.3 `GAnimator_GetClipDuration`

```c
float GAnimator_GetClipDuration(GEntityHandle entity, uint64_t comp_type_hash, int index);
```

作用：片段时长（秒）。失败返回 `-1`。

示例：

```c
float dur = GAnimator_GetClipDuration(e, skinned_hash, 0);
printf("clip 0 lasts %.2fs\n", dur);
```

### 7.4 `GAnimator_GetBoneCount`

```c
int GAnimator_GetBoneCount(GEntityHandle entity, uint64_t comp_type_hash);
```

作用：骨架骨骼数量。失败 `-1`。

示例：

```c
int bones = GAnimator_GetBoneCount(e, skinned_hash);
```

### 7.5 `GAnimator_GetBoneName`

```c
int GAnimator_GetBoneName(GEntityHandle entity, uint64_t comp_type_hash,
                          int bone_index, char* out_buf, int buf_size);
```

作用：取第 `bone_index` 根骨骼名。返回写入长度，失败 `-1`。

示例：

```c
char bn[128];
GAnimator_GetBoneName(e, skinned_hash, 0, bn, sizeof(bn));
```

### 7.6 `GAnimator_GetBoneParentIndex`

```c
int GAnimator_GetBoneParentIndex(GEntityHandle entity, uint64_t comp_type_hash, int bone_index);
```

作用：骨骼的父级下标；`-1` 表示根骨骼，`-2` 表示失败（注意与根骨骼区分）。

示例：

```c
int p = GAnimator_GetBoneParentIndex(e, skinned_hash, i);
if (p >= 0) printf("bone %d parent %d\n", i, p);
```

### 7.7 `GAnimator_GetClipTrackCount`

```c
int GAnimator_GetClipTrackCount(GEntityHandle entity, uint64_t comp_type_hash, int clip_index);
```

作用：片段里的骨骼轨道数量。失败 `-1`。

示例：

```c
int tracks = GAnimator_GetClipTrackCount(e, skinned_hash, 0);
```

### 7.8 `GAnimator_GetClipTrackBone`

```c
int GAnimator_GetClipTrackBone(GEntityHandle entity, uint64_t comp_type_hash,
                               int clip_index, int track_index);
```

作用：轨道对应的骨骼下标。失败 `-1`。

示例：

```c
int bone = GAnimator_GetClipTrackBone(e, skinned_hash, 0, 0);
```

### 7.9 `GAnimator_GetClipKeyframeCount`

```c
int GAnimator_GetClipKeyframeCount(GEntityHandle entity, uint64_t comp_type_hash,
                                   int clip_index, int track_index, int channel);
```

作用：轨道指定通道（`0` 平移 / `1` 旋转 / `2` 缩放）的关键帧数量。失败 `-1`。

示例：

```c
int keys = GAnimator_GetClipKeyframeCount(e, skinned_hash, 0, 0, 1);
```

### 7.10 `GAnimator_GetClipKeyframe`

```c
int GAnimator_GetClipKeyframe(GEntityHandle entity, uint64_t comp_type_hash,
                              int clip_index, int track_index, int channel,
                              int key_index, float* out, int out_capacity);
```

作用：取一个关键帧。`out[0] = time`；平移/缩放 `out[1..3] = XYZ`；旋转 `out[1..4] = XYZW`。

返回：写入的 float 数量（4 或 5）；失败 `-1`。

示例：

```c
float kf[5];
int n = GAnimator_GetClipKeyframe(e, skinned_hash, 0, 0, 1, 0, kf, 5);
if (n == 5) printf("t=%.3f q=(%.2f, %.2f, %.2f, %.2f)\n", kf[0], kf[1], kf[2], kf[3], kf[4]);
```

---

## 8. 编辑命令面（edit_api.h）

这一组是**唯一**把用户编辑写进场景的通道：全部通过命令队列在下一帧执行，并自动进撤销栈（Play 模式冻结）。运行期逻辑（相机、程序化运动、AI）请用 2.11 的直接写入接口，不要用这里的函数。

约定：

* 返回 `1` 表示已入队，`0`/`-1` 表示失败；
* 字符串字段定长，超长截断并保证 `\0` 结尾；
* 命令执行失败只写日志，不回调。

### 8.1 `GEdit_CreateEntity`

```c
int GEdit_CreateEntity(const char* name, GEntityHandle parent);
```

作用：创建实体并挂到 `parent` 下（0 = 场景根）。创建后自动选中（"建完就能直接改属性"）。返回 `1` 已入队。

示例：

```c
GEdit_CreateEntity("Enemy", root);
```

### 8.2 `GEdit_DestroyEntity`

```c
int GEdit_DestroyEntity(GEntityHandle entity);
```

作用：删除实体及其全部子孙。

示例：

```c
GEdit_DestroyEntity(GEntity_GetSelected());
```

### 8.3 `GEdit_RenameEntity`

```c
int GEdit_RenameEntity(GEntityHandle entity, const char* name);
```

作用：改名（触发 `OnEntityListChanged`）。

示例：

```c
GEdit_RenameEntity(e, "Player");
```

### 8.4 `GEdit_ReparentEntity`

```c
int GEdit_ReparentEntity(GEntityHandle entity, GEntityHandle parent);
```

作用：改父级，`parent = 0` 表示挂到场景根。世界变换保持不变（局部变换被反算）。

示例：

```c
GEdit_ReparentEntity(e, GEntity_GetChildAt(root, 0));
```

### 8.5 `GEdit_SelectEntity`

```c
int GEdit_SelectEntity(GEntityHandle entity);
```

作用：设置当前选中（`0` 表示清空选择）。触发 `OnEntitySelected` / `OnEntityDeselected`。

示例：

```c
GEdit_SelectEntity(hit ? hit : 0);
```

### 8.6 `GEdit_SetTransform`

```c
int GEdit_SetTransform(GEntityHandle entity, const GVec3* position,
                       const GQuat* rotation, const GVec3* scale);
```

作用：一次性写局部 TRS（Gizmo 拖拽、Inspector 数值框、撤销恢复都走这条）。`rotation` 传 `{0,0,0,0}` 表示保持原旋转。

示例：

```c
GVec3 p = {1.0f, 0.0f, 2.0f};
GQuat q = {0.0f, 0.0f, 0.0f, 0.0f};   // 保持原旋转
GVec3 s = {1.0f, 1.0f, 1.0f};
GEdit_SetTransform(e, &p, &q, &s);
```

### 8.7 `GEdit_TranslateEntity`

```c
int GEdit_TranslateEntity(GEntityHandle entity, const GVec3* delta);
```

作用：局部空间**增量**平移。执行时才读当前位置，因此同一帧内连续调用可以叠加；"先设绝对位置再微调"必须用这条，不能两次 `SetTransform`（第二条入队时读到的还是旧位置，会把第一条覆盖掉）。

示例：

```c
GVec3 d = {0.0f, 0.0f, 0.1f};
GEdit_TranslateEntity(e, &d);
GEdit_TranslateEntity(e, &d);   // 同帧两次 = 前进 0.2
```

### 8.8 `GEdit_SetProperty`

```c
int GEdit_SetProperty(GEntityHandle entity, uint64_t type_hash,
                      const char* prop_name, const void* value, int value_size);
```

作用：按反射字段写属性（原始字节；字符串传 C 字符串）。与 3.7 的区别：这条进撤销栈、下一帧生效。

示例：

```c
float fov = 75.0f;
GEdit_SetProperty(camera, camera_hash, "fov", &fov, sizeof(fov));
```

### 8.9 `GEdit_AddComponent`

```c
int GEdit_AddComponent(GEntityHandle entity, const char* type_name);
```

作用：按类型名（短名或全名）添加组件。

示例：

```c
GEdit_AddComponent(e, "ColorRect");
```

### 8.10 `GEdit_RemoveComponent`

```c
int GEdit_RemoveComponent(GEntityHandle entity, uint64_t type_hash);
```

作用：按类型 hash 删除组件（同类型多实例时删除第一个匹配项）。

示例：

```c
GEdit_RemoveComponent(e, color_rect_hash);
```

### 8.11 `GEdit_SetMaterial`

```c
int GEdit_SetMaterial(GEntityHandle entity, const char* mesh_path,
                      float r, float g, float b,
                      float roughness, float metallic);
```

作用：给实体（无则加）`MeshRenderer` 并设置网格与 PBR 参数，进撤销栈。`mesh_path` 为空表示只改材质。

示例：

```c
GEdit_SetMaterial(e, "res:/models/cube.obj", 0.8f, 0.8f, 0.9f, 0.2f, 0.9f);
```

### 8.12 `GEdit_SetRenderTargetSize`

```c
int GEdit_SetRenderTargetSize(int target, int width, int height);
```

作用：请求改变离屏目标尺寸（`0` 场景视口 / `1` 游戏视口）。渲染侧通过 `GCore_TakeRenderTargetRequest` 取走。

示例：

```c
GEdit_SetRenderTargetSize(0, 1280, 720);
```

### 8.13 `GEdit_SaveScene`

```c
int GEdit_SaveScene(const char* path);
```

作用：请求存档（下一帧执行）。与 `GScene_Save` 的区别是走命令队列，能与其它编辑命令保持顺序。

示例：

```c
GEdit_SaveScene("res:/scenes/main.gesc");
```

### 8.14 `GEdit_ImportAsset`

```c
int GEdit_ImportAsset(const char* path);
```

作用：请求导入资产（触发导入、生成侧车、刷新实体列表）。

示例：

```c
GEdit_ImportAsset("res:/models/new_prop.fbx");
```

### 8.15 `GEdit_SetPlayMode`

```c
int GEdit_SetPlayMode(int mode);
```

作用：`0` = 停止（回编辑态）、`1` = 进入 Play、`2` = 切换暂停。触发 `OnPlayModeChanged`。

示例：

```c
GEdit_SetPlayMode(GCore_IsPlaying() ? 0 : 1);
```

### 8.16 `GEdit_StepFrame`

```c
int GEdit_StepFrame(int frames);
```

作用：Play 且暂停时前进 `frames` 帧，消费后自动恢复暂停。用于逐帧调试物理/动画。

示例：

```c
if (GCore_IsPlaying() && GCore_IsPaused()) GEdit_StepFrame(1);
```

---

## 附录 A：完整启动示例

下面是一个最小但完整的宿主：初始化 core、载入场景、跑 120 帧、每帧驱动一次模拟、最后按正确顺序收尾。窗口与渲染部分属于 `GrycePlatform` / `GryceRenderer`，见 [integration.md](integration.md) 第 4 节。


```c
#include <stdio.h>
#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/component_api.h"
#include "api/scene_api.h"
#include "api/edit_api.h"

int main(void) {
    GCoreInitDesc desc = {0};
    desc.version = (uint32_t)sizeof(GCoreInitDesc);
    desc.project_root = "D:/MyGame";
    desc.enable_reflection = true;
    if (GCore_Init(&desc) != 0) return 1;

    GCore_SetAutoLoadMainScene(false);
    if (GScene_Load("res:/scenes/main.gesc") != 0) {
        fprintf(stderr, "scene missing\n");
        GCore_Shutdown();
        return 2;
    }

    const float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 120; ++frame) {
        GCore_BeginFrame(dt);

        if (frame == 10) {
            GEdit_CreateEntity("Cube", 0);              // 下一帧生效
        }
        if (frame == 20) {
            GEntityHandle cube = GEntity_GetSelected();
            GVec3 up = {0.0f, 1.0f, 0.0f};
            GEdit_TranslateEntity(cube, &up);
        }
        if (frame == 30) {
            GCore_Undo();                               // 撤销上一步平移
        }

        GCore_EndFrame();
    }

    GCore_Shutdown();
    return 0;
}
```

## 附录 B：命令类型速查（`GCommandType`）

| 值 | 名称 | 说明 | 推荐入口 |
| --- | --- | --- | --- |
| 0 | `ECMD_NOP` | 空命令 | - |
| 1 | `ECMD_LOAD_SCENE` | 载入场景 | `GScene_Load` |
| 2 | `ECMD_SAVE_SCENE` | 存档 | `GEdit_SaveScene` |
| 3 | `ECMD_CREATE_ENTITY` | 建实体 | `GEdit_CreateEntity` |
| 4 | `ECMD_DESTROY_ENTITY` | 删实体 | `GEdit_DestroyEntity` |
| 5 | `ECMD_RENAME_ENTITY` | 改名 | `GEdit_RenameEntity` |
| 6 | `ECMD_REPARENT_ENTITY` | 改父 | `GEdit_ReparentEntity` |
| 7 | `ECMD_SELECT_ENTITY` | 选中 | `GEdit_SelectEntity` |
| 8 | `ECMD_SET_TRANSFORM` | 写 TRS | `GEdit_SetTransform` |
| 9 | `ECMD_TRANSLATE_ENTITY` | 增量平移 | `GEdit_TranslateEntity` |
| 10 | `ECMD_SET_PROPERTY` | 写反射字段 | `GEdit_SetProperty` |
| 11 | `ECMD_ADD_COMPONENT` | 加组件 | `GEdit_AddComponent` |
| 12 | `ECMD_REMOVE_COMPONENT` | 删组件 | `GEdit_RemoveComponent` |
| 13-17 | `ECMD_PLAY_MODE` / `STOP_MODE` / `PAUSE_MODE` / `STEP_FRAME` / `IMPORT_ASSET` | 运行控制 | `GEdit_SetPlayMode` / `GEdit_StepFrame` / `GEdit_ImportAsset` |
| 18 | `ECMD_SET_COMPONENT_PARENT` | 组件树改挂 | 无封装，直接 `PushCommand` |
| 19 | `ECMD_SET_COMPONENT_LOCAL_TRANSFORM` | 组件局部变换 | 无封装，直接 `PushCommand` |
| 100 | `ECMD_SET_RENDER_TARGET` | 离屏目标尺寸 | `GEdit_SetRenderTargetSize` |
| 101-104 | `ECMD_SET_VIEWPORT_SIZE` / `SET_GAMEVIEW_SIZE` / `SET_MATERIAL` | 视口与材质 | `GEdit_SetMaterial` |
| 200-205 | `ECMD_INPUT_*` | 输入事件（按键/鼠标） | `GInput_SyncToCore`（GrycePlatform） |
| 300-301 | `ECMD_PHYSICS_*` | 重力 / 施力 | 组件接口 |
| 400-402 | `ECMD_GIZMO_*` | Gizmo 操作与拖拽 | 渲染侧 |
| 500-501 | `ECMD_SET_SCRIPT` / `RELOAD_SCRIPTS` | 脚本绑定与热重载 | 脚本层 |

payload 布局（`GCmdCreateEntity` / `GCmdSetTransform` / `GCmdSetProperty` 等）定义在 `engine/api/edit_api.h`，用 `GCore_PushCommand` 直接发命令时按对应结构体填 `cmd.payload`（定长 256 字节，编译期有 `static_assert` 守卫）。

## 附录 C：与其它 DLL 的边界

`GryceCore.dll` 只管"数据与逻辑"，不含窗口、输入、渲染：

| 能力 | 所在库 | 头文件 |
| --- | --- | --- |
| 窗口、输入、光标 | `GrycePlatform.dll` | `api/window_api.h`、`api/input_api.h` |
| 渲染器初始化、帧绘制、基础调试 UI | `GryceRenderer.dll` | `api/render_api.h`、`api/viewport_api.h` |
| 原生扩展 ABI | 宿主进程 | `api/extension_api.h` |

宿主同时链接三者时，初始化顺序固定为 **Platform -> Core -> Renderer**，退出顺序相反（Core 先于 Renderer 拆除）。

### 尚未实现的声明

`api/script_api.h` 里有 9 个带 `GRYCE_CORE_API` 的声明（`GScript_GetVersion`、`GScript_RunString`、`GScript_RunFile`、`GScript_GetPropCount`、`GScript_GetPropInfo`、`GScript_GetPropFloat`、`GScript_SetPropFloat`、`GScript_GetPropString`、`GScript_SetPropString`），但**全仓没有任何实现**，`GryceCore.dll` 的导出表里也不存在这些符号，且没有任何文件 include 该头。头文件自身已标注 `Deprecated: use ECS ScriptComponent with QuickJS`，因此它只是历史占位，不要链接、不要调用。

要跑脚本，走 ECS 的 `ScriptComponent`（类型名 `"Script"`，字段是 `script_path` 与 `props`，用 `GComponent_AddComponent` 挂到实体上），由核心在帧循环内驱动，支持热重载；C 侧没有"按字符串直接求值"的导出接口。

本文档覆盖的是 `GryceCore.dll` 实际导出的 **138** 个函数，可用 `objdump -p GryceCore.dll` 核对：`GCore_` 36、`GComponent_` 29、`GEntity_` 24、`GEdit_` 16、`GScene_` 10、`GAsset_` 10、`GAnimator_` 10、`GMaterial_` 3。
