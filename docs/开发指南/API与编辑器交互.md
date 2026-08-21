# Gryce Engine C API 与编辑器交互指南

> 本文档详细说明 C API 层的工作原理、C# P/Invoke 映射方式、命令队列机制，以及编辑器的架构与数据流。

---

## 1. C API 设计原则

### 1.1 为什么用纯 C API

编辑器（C#）与核心（C++）之间通过**纯 C ABI**（`extern "C"` + `cdecl`）通信，原因：

1. **语言无关**：C ABI 是跨语言调用的标准契约
2. **ABI 稳定**：C++ 的 name mangling 在不同编译器间不兼容
3. **模块化**：4 个 DLL 可以独立更新，只要 C API 不变

### 1.2 导出宏

```cpp
// core/export.h
#ifdef GRYCE_CORE_BUILDING
    #define GRYCE_API __declspec(dllexport)  // 编译时导出
#else
    #define GRYCE_API __declspec(dllimport)  // 消费时导入
#endif
```

每个模块有自己的 BUILDING 宏和 API 宏：
- `GRYCE_CORE_BUILDING` → `GRYCE_CORE_API`
- `GRYCE_RENDERER_BUILDING` → `GRYCE_RENDERER_API`
- (Platform 和 Physics 同理)

### 1.3 DLL 命名约定

| 配置 | 核心 DLL 名 | 示例 |
|------|-----------|------|
| Debug | 带 `d` 后缀 | `GryceCored.dll` |
| Release | 无后缀 | `GryceCore.dll` |

C# 端在 `NativeLibrary.cs` 中通过 `#if DEBUG` 条件编译来区分：

```csharp
// editor/src/Native/NativeLibrary.cs
public static class NativeLibrary
{
#if DEBUG
    public const string Core = "GryceCored";
    public const string Renderer = "GryceRendererd";
    // ...
#else
    public const string Core = "GryceCore";
    // ...
#endif
}
```

### 1.4 线程安全保护

所有导出函数由全局递归互斥锁保护：

```cpp
// core/GryceCore/api_guard.h
#define GRYCE_API_GUARD() \
    std::lock_guard<std::recursive_mutex> _api_guard(gryce_core::api_mutex())

// 每个导出函数入口
int GCore_Init(const GCoreInitDesc* desc) {
    GRYCE_API_GUARD();
    // ... 实际逻辑
}
```

递归锁允许同一线程内 API 互相调用、以及回调中再次调用 API。

---

## 2. 命令队列机制

### 2.1 为什么需要命令队列

编辑器对场景的**结构性修改**（创建/销毁实体、改属性、Play Mode 等）不能直接同步执行，因为：

1. 这些操作可能触发场景重建、回调和 UI 刷新
2. 编辑器 UI 线程和引擎线程需要解耦
3. 批量操作可以合并处理

### 2.2 命令缓冲结构

```cpp
// core/runtime/command_buffer.h
class CommandBuffer {
    static constexpr int kCapacity = 8192;
    GCommand buffers_[2][kCapacity];  // 双缓冲
    std::atomic<int> write_index_;    // 当前写入缓冲索引
    int read_index_;                   // 当前读取缓冲索引
    // ...
};
```

### 2.3 命令生命周期

```
Editor 线程                              Core 线程（GCore_BeginFrame）
    │                                          │
    │  GCore_PushCommand(cmd)                   │
    │  ─────────────────────────────────────►    │
    │  无锁写入 front buffer                     │
    │                                          │
    │                                          │  swap buffers
    │                                          │  ▼
    │                                          │  消费 back buffer（≤30 条/帧）
    │                                          │  ├─ ECMD_CREATE_ENTITY
    │                                          │  ├─ ECMD_DESTROY_ENTITY
    │                                          │  ├─ ECMD_SET_PROPERTY
    │                                          │  └─ ...
    │                                          │  溢出回队
    │                                          │
    │  GCore_EndFrame()                         │
    │  ◄─────────────────────────────────────    │
    │  触发延迟回调                               │
    │  (OnSceneLoaded / OnEntityListChanged)     │
```

### 2.4 命令类型与载荷

```c
// 核心命令（值 1-15）
ECMD_LOAD_SCENE       // payload: char path[256]
ECMD_CREATE_ENTITY    // payload: { char name[128]; GEntityHandle parent; }
ECMD_DESTROY_ENTITY   // payload: GEntityHandle h
ECMD_RENAME_ENTITY    // payload: { GEntityHandle h; char name[128]; }
ECMD_REPARENT_ENTITY  // payload: { GEntityHandle h; GEntityHandle parent; }
ECMD_SELECT_ENTITY    // payload: GEntityHandle h
ECMD_SET_TRANSFORM    // payload: { GEntityHandle h; GVec3 pos; GQuat rot; GVec3 scale; }
ECMD_SET_PROPERTY     // payload: { GEntityHandle h; uint64_t type_hash; char prop_name[64]; uint8_t value[128]; }
ECMD_ADD_COMPONENT    // payload: { GEntityHandle h; char type_name[128]; }
ECMD_REMOVE_COMPONENT // payload: { GEntityHandle h; uint64_t type_hash; }
ECMD_PLAY_MODE        // 无载荷
ECMD_STOP_MODE        // 无载荷
ECMD_PAUSE_MODE       // 无载荷

// 渲染命令（值 100+）
ECMD_SET_RENDER_TARGET  // 未实现
ECMD_SET_VIEWPORT_SIZE  // 未实现

// 输入命令（值 200+）
ECMD_INPUT_KEY          // 已弃用，改用 GInput_Inject*
ECMD_INPUT_MOUSE_MOVE   // 已弃用

// 物理命令（值 300+）
ECMD_PHYSICS_SET_GRAVITY  // 已弃用，改用 GPhysics_SetGravity

// Gizmo 命令（值 400+）
ECMD_GIZMO_SET_OPERATION  // 占位
ECMD_GIZMO_SET_SPACE      // 占位
ECMD_GIZMO_MANIPULATE     // 占位

// 脚本命令（值 500+）
ECMD_SET_SCRIPT       // Phase 1/2
ECMD_RELOAD_SCRIPTS   // Phase 1/2
```

### 2.5 直接 API vs 命令 API

| 场景 | 使用方式 | 示例 |
|------|---------|------|
| 创建实体 | 命令 | `GCore_PushCommand(ECMD_CREATE_ENTITY, ...)` |
| 删除实体 | 命令 | `GCore_PushCommand(ECMD_DESTROY_ENTITY, ...)` |
| 修改属性 | 命令 | `GCore_PushCommand(ECMD_SET_PROPERTY, ...)` |
| 进入 Play Mode | 命令 | `GCore_PushCommand(ECMD_PLAY_MODE, ...)` |
| 枚举实体 | 直接 API | `GEntity_GetCount()` / `GEntity_GetAt(i)` |
| 读实体名 | 直接 API | `GEntity_GetName(handle, buf, size)` |
| 读属性 | 直接 API | `GComponent_GetProperty(...)` |
| 写属性（Inspector） | 直接 API | `GComponent_SetProperty(...)` |
| 导入资源 | 直接 API | `GAsset_Import(path)` |

---

## 3. C# P/Invoke 映射

### 3.1 类型映射

| C 类型 | C# 类型 | 说明 |
|--------|---------|------|
| `int` | `int` | 基础整数 |
| `bool` | `[MarshalAs(UnmanagedType.U1)] bool` | C# 中 bool 占 4 字节，需显式指定 |
| `float` | `float` | 单精度浮点 |
| `char*` (输入) | `string` | UTF-8 字符串 |
| `char*` (输出) | `StringBuilder` | 预分配缓冲区 |
| `void*` | `IntPtr` / `nint` | 不透明指针 |
| `uint8_t*` | `byte[]` / `Span<byte>` | 字节数组 |
| `struct` (输入) | `ref Struct` | 结构体引用 |
| 函数指针 | `delegate` | 需 `[UnmanagedFunctionPointer(Cdecl)]` |

### 3.2 字符串处理

C API 使用 UTF-8，而 C# 默认使用 UTF-16。编辑器中的 `EntityAPI.cs` 包装了字符串转换：

```csharp
// editor/src/Native/EntityAPI.cs
public static string? GetNameUtf8(GEntityHandle handle)
{
    byte[] buf = new byte[256];
    fixed (byte* p = buf)
    {
        int len = GEntity_GetName(handle, (IntPtr)p, buf.Length);
        return len > 0 ? Encoding.UTF8.GetString(buf, 0, len) : null;
    }
}
```

### 3.3 回调委托

C# 端需要保持回调委托存活（防止 GC 回收）：

```csharp
// 委托声明（必须保持全局引用）
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
private delegate void GOnEntityListChanged(IntPtr userData);

// 保持存活的字段
private readonly GOnEntityListChanged _onEntityListChanged;

// 注册
_onEntityListChanged = userData => {
    // 注意：此回调在引擎线程调用，需 Dispatcher.Invoke 到 UI 线程
    Application.Current.Dispatcher.Invoke(() => { ... });
};
CoreAPI.GCore_RegisterCallback_OnEntityListChanged(_onEntityListChanged);
```

### 3.4 命令构造

```csharp
// 写入实体变换命令
public static void PushSetTransform(GEntityHandle handle, Vector3 pos, Quaternion rot, Vector3 scale)
{
    unsafe
    {
        var payload = new byte[sizeof(int) + sizeof(float) * 10];
        fixed (byte* p = payload)
        {
            int offset = 0;
            *(int*)(p + offset) = (int)handle; offset += sizeof(int);
            ((GVec3*)(p + offset))->Set(pos.X, pos.Y, pos.Z); offset += sizeof(GVec3);
            ((GQuat*)(p + offset))->Set(rot.X, rot.Y, rot.Z, rot.W); offset += sizeof(GQuat);
            ((GVec3*)(p + offset))->Set(scale.X, scale.Y, scale.Z);
        }
        var cmd = GCommand.Create(GCommandType.SetTransform, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }
}
```

---

## 4. 编辑器架构

### 4.1 整体架构

```
WPF 应用（App.xaml.cs）
    │
    ├── MainWindow.xaml
    │   ├── ToolbarView（工具栏）
    │   ├── HierarchyView（层级面板）
    │   ├── ViewportView（视口，内嵌 GL HWND）
    │   ├── InspectorView（检视面板）
    │   ├── ProjectView（资源面板）
    │   ├── ConsoleView（控制台）
    │   └── AnimationPanelView（动画面板）
    │
    ├── EditorViewModel（主 ViewModel）
    │   ├── 回调注册（OnEntityListChanged 等）
    │   ├── Hierarchy 刷新（增量缓存）
    │   ├── Inspector 刷新
    │   ├── Undo/Redo 栈
    │   └── 命令推送
    │
    └── EngineService（引擎服务）
        ├── 初始化（GCore_Init → GPhysics_Init → GWindow_Init → GRender_Init）
        ├── 60Hz 帧驱动（DispatcherTimer → GCore_BeginFrame/EndFrame）
        ├── 自动保存
        └── 项目切换
```

### 4.2 初始化流程

```csharp
// editor/src/Services/EngineService.cs
public void Initialize(string projectRoot)
{
    // 1. 初始化核心
    var desc = new GCoreInitDesc {
        Version = (uint)Marshal.SizeOf<GCoreInitDesc>(),
        ProjectRoot = resolvedRoot,
        EnableReflection = true
    };
    CoreAPI.GCore_Init(ref desc);

    // 2. 初始化物理系统
    nint worldPtr = CoreAPI.GCore_GetInternalWorldPtr();
    PhysicsAPI.GPhysics_Init(GPhysicsBackend.Jolt);
    PhysicsAPI.GPhysics_AttachSystems(worldPtr);

    // 3. 启动帧循环
    IsInitialized = true;
    _frameTimer.Start();  // 60Hz DispatcherTimer
}
```

### 4.3 每帧流程

```csharp
// EngineService 的 DispatcherTimer 回调
private void OnFrameTick(object? sender, EventArgs e)
{
    if (!IsInitialized) return;

    // 1. 同步输入（游戏视图激活时）
    if (ViewportView.GameViewActive)
        Native.InputAPI.GInput_SyncToCore();

    // 2. 引擎帧（消费命令 + Play Mode 更新 + 延迟回调）
    CoreAPI.GCore_BeginFrame(dt);
    CoreAPI.GCore_EndFrame();

    // 3. 处理延迟保存
    FlushDeferredSave();

    // 4. 更新 Play Mode 状态
    IsPlaying = CoreAPI.GCore_IsPlaying();
    IsPaused = CoreAPI.GCore_IsPaused();
}
```

### 4.4 渲染线程流程

```csharp
// 渲染线程（由 ViewportView 的 GL 上下文驱动）
// 每帧：
GWindow_MakeContextCurrent();       // 绑定 GL 上下文
GViewport_SetSize(w, h);            // 更新视口尺寸
GRender_BeginFrame();               // 开始帧
GRender_RenderWorld();              // 渲染场景
GRender_RenderGizmo();              // 渲染 Gizmo（占位）
GRender_RenderGameView();           // 渲染 GameView
GRender_EndFrame();                 // 结束帧（swap 在锁外执行）
GWindow_ReleaseContext();           // 释放 GL 上下文
```

### 4.5 回调系统

编辑器通过回调接收引擎事件：

| 回调 | 触发时机 | 编辑器处理 |
|------|---------|-----------|
| `OnEntityListChanged` | 实体增删/改名/重挂后下一帧 | 刷新 Hierarchy 树 |
| `OnEntitySelected` | 选中实体时 | 更新 Inspector |
| `OnEntityDeselected` | 取消选中时 | 清空 Inspector |
| `OnPlayModeChanged` | 播放/停止/暂停变化 | 更新工具栏状态 |
| `OnLogMessage` | 每帧 EndFrame 转发 | 追加到 Console |
| `OnSceneLoaded` | 场景加载/新建成功 | 更新标题栏、清空脏标记 |
| `OnComponentChanged` | 组件变化 | 刷新 Inspector |

### 4.6 Undo/Redo 系统

编辑器实现了完整的 Undo/Redo，位于 `editor/src/ViewModels/EditorViewModel.Undo.cs`：

```csharp
// 支持的操作类型
interface IUndoableAction {
    void Undo(EditorViewModel vm);
    void Redo(EditorViewModel vm);
}

class CreateEntityAction : IUndoableAction { ... }
class DeleteEntityAction : IUndoableAction { ... }
class TransformAction : IUndoableAction { ... }
class PropertyAction : IUndoableAction { ... }
class RenameAction : IUndoableAction { ... }
class ReparentAction : IUndoableAction { ... }
class ComponentAction : IUndoableAction { ... }
class PasteAction : IUndoableAction { ... }
class EntityTypeAction : IUndoableAction { ... }
class MultiAction : IUndoableAction { ... }  // 批量操作
```

关键点：
- `_undoStack` / `_redoStack` 两个栈
- 创建实体时使用 `_pendingCreateActions` 队列延迟绑定句柄
- `_pendingComponentRestores` 等待组件添加命令完成后再恢复属性
- 连续快速创建多个实体时，每个 action 按序绑定句柄

### 4.7 多选支持

编辑器支持多选（Ctrl/Shift 点击），用于批量复制/删除：

```csharp
// _multiSelection HashSet 存储多选实体
// TopmostSelection() 过滤掉祖先也在选中集合中的实体（避免重复操作）
// 复制/删除时遍历 TopmostSelection 而非全集合
```

---

## 5. 编辑器面板详解

### 5.1 Hierarchy 面板

文件：`editor/src/Views/HierarchyView.xaml` / `.cs`

- 显示实体层级树
- 支持：重命名、重挂、删除、复制/粘贴、导出/导入 JSON
- 增量刷新：`_entityModelCache` 按 handle 缓存 EntityModel，避免每次重建
- 组件签名缓存：`_entityComponentSignature` 记录组件数量，只有变化时才刷新组件列表
- 集合同步：`SyncCollection()` 只在内容变化时才替换，保留 WPF 展开状态

### 5.2 Inspector 面板

文件：`editor/src/Views/InspectorView.xaml` / `.cs`

- 通过反射 C API 动态读写组件属性
- 支持：Transform 编辑、材质 PBR 参数、贴图槽、AO/自发光/UV
- 只读字段（Camera 的 FOV/Near/Far 等）灰显
- 材质编辑器入口（右键 MeshRenderer/SkinnedMeshRenderer）

### 5.3 Viewport 面板

文件：`editor/src/Views/ViewportView.xaml` / `.cs`（多个分部类）

- 内嵌 GLFW 子窗口（`ViewportHwndHost.cs`）
- 相机控制：鼠标拖动旋转/平移/缩放
- Gizmo 拖动：平移/旋转/缩放（`ViewportView.Gizmo.cs`）
- 2D 编辑模式：临时相机，不保存到场景文件
- 网格对齐：`SnapDelta` 方法
- 渲染表面恢复：`GRender_RequestSurfaceRecreate`（标签切换后重建）

### 5.4 Project 面板

文件：`editor/src/Views/ProjectView.xaml` / `.cs`

- 资源树（文件夹、场景、模型、纹理、材质文件）
- 水平网格布局，动态高度（根据文件名行数）
- 图标：灰色文件夹、白色文档（带类型颜色指示器）
- 中文文件名支持

### 5.5 Console 面板

文件：`editor/src/Views/ConsoleView.xaml` / `.cs`

- 通过 `GOnLogMessage` 回调接收引擎日志
- 每帧 `GCore_EndFrame` 统一转发
- 日志级别：Info / Warning / Error

### 5.6 动画编辑器

文件：`editor/src/Views/AnimationPanelView.xaml` / `.cs`、`AnimationEditorWindow.xaml` / `.cs`

- 片段列表、播放控制（播放/暂停/循环/速度）
- 骨骼层级树
- 骨骼轨道 P/R/S 关键帧表格
- 关键帧 JSON 导出（`.anim.json`）
- C API 支持：`GAnimator_GetBone*`、`GAnimator_GetClipTrack*`、`GAnimator_GetClipKeyframe*`

### 5.7 材质编辑器

文件：`editor/src/Views/MaterialEditorWindow.xaml` / `.cs`

- PBR 参数：albedo / normal / roughness / metallic / ao / emissive
- 6 张贴图槽 + 颜色参数
- 修改即时生效（`GMaterial_SetField`）
- 支持加载 `.gmat` 文件

### 5.8 脚本编辑器

文件：`editor/assets/monaco/editor.html`（Monaco Editor）

- Undo/Redo 转发（Ctrl+Z/Y/Shift+Z、工具栏按钮）
- 未保存脏标记（标签前 `*`）
- 离开/换文件时的保存确认

---

## 6. 数据流范例

### 6.1 创建实体

```
1. 用户点击 Create Entity 按钮
2. EditorViewModel.CreateEntity() 构造命令载荷
3. EngineService.PushCommand(ECMD_CREATE_ENTITY, payload)
4. CoreAPI.GCore_PushCommand() 写入命令缓冲
5. 下一帧 GCore_BeginFrame() 消费命令：
   - scene->create_entity(name)
   - 生成新 UUID 和 EntityHandle
   - 标记实体列表变化
6. GCore_EndFrame() 触发 OnEntityListChanged 回调
7. EditorViewModel 收到回调 → Dispatcher.Invoke
8. RefreshHierarchy() 重建实体树
9. SelectPendingNewEntity() 选中新实体
```

### 6.2 Play Mode

```
1. 用户点击 Play 按钮
2. EngineService.Play() → PushCommand(ECMD_PLAY_MODE)
3. GCore_BeginFrame 消费命令：
   - 序列化当前场景为 JSON 快照（play_snapshot_json）
   - world.set_updates_enabled(true)
   - 设置 play_mode = true
4. 每帧 GCore_BeginFrame 调用 world.update(dt)：
   - ScriptSystem.on_update：按优先级执行 Lua 脚本
   - PhysicsSystem3D/2D：步进物理世界
   - AnimatorSystem：更新骨骼动画
5. 用户点击 Stop 按钮
6. PushCommand(ECMD_STOP_MODE)
7. GCore_BeginFrame 消费命令：
   - 反序列化快照 JSON → attach_scene（替换场景，丢弃播放期改动）
   - world.set_updates_enabled(false)
   - 设置 play_mode = false
```

### 6.3 资源加载与渲染

```
1. 场景加载时，MeshRenderer 引用模型路径
2. RenderSystem3D 遍历场景，收集待渲染实体
3. 对每个实体：
   - AssetManager 根据路径缓存 mesh/texture
   - 若未加载 → AsyncLoader 异步加载
   - 同步模式下每帧上传预算 30 个网格
4. RenderPipeline 处理：
   - 阴影贴图（CSM）
   - PBR 渲染
   - 后处理（HDR → Bloom → Tonemapping → 调色）
5. IRenderBackend 提交 GPU 命令
6. swap buffers → 显示
```

---

## 7. 常见开发任务

### 7.1 添加新组件

1. 在 `core/components/` 下创建组件类（继承 `Component`）
2. 实现 `serialize()` / `deserialize()` / `clone()` / `type()`
3. 在 `core/components/component_factory.cpp` 注册
4. 在 `core/reflection/builtin_reflections.cpp` 添加反射
5. 在 `core/CMakeLists.txt` 的 GryceCore PUBLIC 头文件列表中添加组件头文件
6. 在 `core/api/core_api.cpp` 的 `register_components()` 中注册组件
7. C# 端无需额外代码（组件自动出现在 Add Component 对话框）

### 7.2 添加新 C API 函数

1. 在对应的 `core/Gryce*/xxx_api.h` 中添加声明
2. 在 `core/api/xxx_api.cpp` 中添加实现
3. 在 `editor/src/Native/XxxAPI.cs` 中添加 P/Invoke 声明
4. 如果需要，在 `EditorViewModel.cs` 或 `EngineService.cs` 中添加包装方法

### 7.3 添加新渲染功能

1. 如果是后端无关的：在 `core/render/` 中添加
2. 如果是 OpenGL 专属：在 `core/render/opengl/` 中添加
3. 如果是 Vulkan 专属：在 `core/render/vulkan/` 中添加
4. 着色器放在 `examples/common/shaders/` 下
5. 在 `cmake/shaders.cmake` 中注册新着色器

### 7.4 调试技巧

- **日志**：使用 `GLOG_INFO` / `GLOG_WARN` / `GLOG_ERROR` 宏
- **断点**：C++ 端在 `core/api/` 的 C API 实现中设置断点
- **C# 断点**：在 `editor/src/Native/` 的 P/Invoke 调用处设置
- **命令流**：在 `process_command()` 中设置断点查看命令执行
- **渲染线程**：渲染线程独立，断点需在渲染线程相关函数中设置

---

## 8. 已知问题与限制

| 问题 | 说明 |
|------|------|
| `GEntity_GetWorldPosition/Rotation/Scale` | 未实现，恒返回 `-1` |
| `GRender_RenderGizmo` | 占位实现 |
| `GRender_RenderGameView` | 与 SceneView 共用管线/纹理 |
| `GPhysics_CreateBody` 实体绑定 | 未从实体 Transform 同步初始位置 |
| `GPhysics_Raycast` 实体映射 | `out_entity` 恒为 `0` |
| 编辑器 Viewport 仅 OpenGL 后端 | Vulkan 编辑器集成待跟进 |
| 无独立 GameView FBO | 当前与 SceneView 共用 |
| 大规模场景优化 | GPU Instancing 未启用 |