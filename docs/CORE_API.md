# Gryce Engine — Core API Specification

> 本文档定义引擎核心的公共接口：**模块化 DLL 的 C API**（编辑器与外部工具集成）
> 与引擎内部 C++ API（引擎扩展开发）。C API 是 Editor ↔ Core 之间唯一的公共边界。

---

## 1. 模块化 DLL 与 C API

引擎核心按模块拆分为 4 个 DLL（见 `core/CMakeLists.txt`），模块之间及对外部
（WPF 编辑器、CLI、测试）的唯一公共边界是 **C API**（`extern "C"` 导出，
`__declspec(dllexport)` / `__attribute__((visibility("default")))`）：

| DLL | 导出宏 | 职责 |
|---|---|---|
| `GryceCore.dll` | `GRYCE_CORE_API` | ECS、场景/实体/组件、反射、资源管线、动画与碎裂系统 |
| `GryceRenderer.dll` | `GRYCE_RENDERER_API` | 渲染后端、视口/游戏视图 |
| `GrycePlatform.dll` | `GRYCE_PLATFORM_API` | GLFW 窗口（外部 HWND 附着）、输入注入 |
| `GrycePhysics.dll` | `GRYCE_PHYSICS_API` | Jolt/Box2D 物理世界 + 物理系统注册 |

Debug 构建产物带 `d` 后缀（`GryceCored.dll` 等），Release 为原名。

头文件约定：

- `core/GryceCore/*.h`、`core/GryceRenderer/*.h`、`core/GrycePlatform/*.h`、
  `core/GrycePhysics/*.h`：**纯 C 公共接口**，跨语言（C# P/Invoke）唯一可靠边界。
- `core/` 其余目录（`scene/`、`ecs/`、`components/`、`render/` 等）：引擎内部
  C++ 接口（`gryce_engine` 命名空间），仅面向引擎扩展开发，不对外部消费者保证稳定。

---

## 2. 构建与集成

### 2.0 构建环境

| 项 | 要求 |
|---|---|
| 平台 | Windows 10/11 |
| 编译器 | **MinGW-w64 GCC**（推荐 MSYS2 UCRT64）或 **MSVC**（VS 2022+ / VS 2026） |
| CMake | ≥ 3.28 |
| 生成器 | Ninja（推荐） |

> **注意**：在默认 Windows PowerShell / CMD 中运行 CMake 时，若未安装 MSYS2 或未将 MinGW 加入 PATH，CMake 可能自动检测到 MSVC 并因缺少 `rc.exe` / `mt.exe` 而失败。请使用以下任一方式：
> 1. **推荐**：在 MSYS2 UCRT64 终端中构建。
> 2. 显式指定 MinGW 编译器：`-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`
> 3. 若使用 MSVC，必须在 **x64 Native Tools Command Prompt for VS 2022** 中运行。

根 CMakeLists.txt 已内置自动检测：如果未指定编译器，会尝试在 `C:/msys64/ucrt64/bin` 或 `C:/msys64/mingw64/bin` 中自动找到 MinGW GCC 并锁定。

### 2.1 构建产物

推荐使用一键构建脚本：

```powershell
python build.py            # Debug（默认）
python build.py Release
```

产物输出到 `build/bin/{Debug,Release}/`：

```
GryceCore.dll / GryceCored.dll
GryceRenderer.dll / GryceRendererd.dll
GrycePlatform.dll / GrycePlatformd.dll
GrycePhysics.dll / GrycePhysicsd.dll
glfw3.dll / glfw3d.dll
3dtest.exe / gt2dDemo.exe / gryce_tests.exe
```

### 2.2 编辑器集成

WPF 编辑器（`editor/GryceEngine.Editor.csproj`，.NET Framework 4.8，x64）通过
`NativeDll` ItemGroup 把上述 DLL 复制到输出目录（`editor/src/Native/NativeLibrary.cs`
按 `DEBUG` 符号选择带 `d` 后缀的 DLL），再经 `editor/src/Native/*.cs` 的 P/Invoke
声明调用 C API。P/Invoke 约定：

- `CallingConvention.Cdecl`；
- C 结构体用 `[StructLayout(LayoutKind.Sequential)]` 一比一复刻（见 `Types.cs`）；
- 字符串按 UTF-8 `LPStr` 编组，缓冲类函数（如 `GEntity_GetName`）传
  `StringBuilder` + 容量；
- 回调函数指针用 `[UnmanagedFunctionPointer(CallingConvention.Cdecl)]` delegate，
  且 C# 侧必须持有 delegate 字段防 GC。

---

## 3. 模块化 C API（编辑器 ↔ Core 桥接）

> 本节是 Editor ↔ Core 通信的规范描述。实现位于 `core/api/*.cpp`，
> 编辑器侧一一对应的 P/Invoke 包装位于 `editor/src/Native/*.cs`。

### 3.1 公共类型（`core/GryceCore/types.h`）

**句柄**（不透明整数/指针，编辑器永远接触不到引擎内部对象）：

| 类型 | 底层 | 说明 |
|---|---|---|
| `GEntityHandle` | `int` | 实体句柄（0 = null），经 `EntityHandleMap` 映射到 UUID |
| `GComponentHandle` | `int` | 组件句柄（当前以 `{entity, type_hash}` 组合寻址） |
| `GAssetHandle` | `int` | 资源句柄 |
| `GWindowHandle` / `GTextureHandle` | `void*` | 原生窗口 / 纹理句柄 |
| `GBodyHandle` | `int` | 物理刚体句柄 |

**数学结构**：`GVec3` / `GVec4` / `GQuat` / `GMat4` / `GColor`（`LayoutKind.Sequential` 可直接编组）。

**枚举**：`GRenderAPI`（OpenGL/Vulkan/DX11/DX12）、`GWindowMode`、`GInputAction`、`GPhysicsBackend`。

**命令结构**：

```c
typedef struct {
    GCommandType type;          // 见 3.3
    uint64_t     seq;
    uint8_t      payload[256];  // 各命令的负载布局，见 3.3
} GCommand;
```

### 3.2 模块 API 总览

| 模块 | API 族 | 主要函数 |
|---|---|---|
| GryceCore | `GCore_*` | `GCore_Init/Shutdown`、`GCore_BeginFrame/EndFrame`、`GCore_PushCommand(s)`、`GCore_IsPlaying/IsPaused`、`GCore_RegisterCallback_*`、`GCore_GetLogMessages` |
| GryceCore | `GEntity_*` | `GEntity_GetCount/GetAt/GetName/GetPath`、`GetParent/GetChildAt`、`Get/SetLocalPosition/Rotation/Scale`、`GetWorldPosition`、`ExportJson/ImportJson`（实体子树 JSON 导出/导入，Undo/Prefab 基础）、`SaveAsPrefab/CreatePrefabInstance/ApplyPrefab/RevertPrefab` |
| GryceCore | `GComponent_*` | `GetPropertyCount/GetPropertyInfo/GetProperty/SetProperty`、`Add/RemoveComponent`、`GetRegisteredTypeCount/GetRegisteredTypeInfo` |
| GryceCore | `GScene_*` | `GScene_Load/Save/New/GetCurrentPath`、`PickScreen/PickRay`（网格世界 AABB 射线拾取） |
| GryceCore | `GAsset_*` | `GAsset_Import/Load/GetPath/Unload` |
| GryceCore | `GMaterial_*` | `GMaterial_GetField/SetField`（`GMaterialField` 枚举按字段寻址）、`LoadFromFile`（.gmat 应用到渲染组件） |
| GryceCore | `GAnimator_*` | `GAnimator_GetClipCount/GetClipName/GetClipDuration` |
| GryceRenderer | `GRender_*` | `GRender_Init/Shutdown`、`GRender_BeginFrame/EndFrame`、`GRender_RenderWorld/RenderGameView/RenderGizmo` |
| GryceRenderer | `GViewport_*` | `GViewport_SetSize/GetSize/SetCamera`、`GGameView_SetSize/SetCamera` |
| GrycePlatform | `GWindow_*` | `GWindow_InitExternal(hwnd)`、`GWindow_GetRenderHandle`、`GWindow_SetSize/ShouldClose` |
| GrycePlatform | `GInput_*` | `GInput_InjectKey/MouseMove/MouseButton/MouseScroll`、`GInput_IsKeyHeld` 等查询 |
| GrycePhysics | `GPhysics_*` | `GPhysics_Init/Shutdown`、`GPhysics_AttachSystems(world_ptr)`、`GPhysics_Step/SetGravity`、`CreateBody/SetBodyTransform`、`Raycast` |

### 3.3 命令协议

`GCommandType` 按功能分组（编号见 `types.h`）：

| 分组 | 命令 |
|---|---|
| 0~99 场景/实体编辑 | `LOAD_SCENE`、`SAVE_SCENE`、`CREATE_ENTITY`、`DESTROY_ENTITY`、`RENAME_ENTITY`、`REPARENT_ENTITY`、`SELECT_ENTITY`、`SET_TRANSFORM`、`SET_PROPERTY`、`ADD/REMOVE_COMPONENT`、`PLAY/STOP/PAUSE_MODE`、`STEP_FRAME`、`IMPORT_ASSET` |
| 100~199 渲染 | `SET_RENDER_TARGET`、`SET_VIEWPORT_SIZE`、`SET_GAMEVIEW_SIZE`、`SET_MATERIAL` |
| 200~299 输入 | `INPUT_KEY`、`INPUT_MOUSE_MOVE`、`INPUT_MOUSE_BUTTON`、`INPUT_MOUSE_SCROLL` |
| 300~399 物理 | `PHYSICS_SET_GRAVITY`、`PHYSICS_ADD_FORCE` |
| 400~499 Gizmo | `GIZMO_SET_OPERATION`、`GIZMO_SET_SPACE`、`GIZMO_MANIPULATE` |

**执行模型**：命令进入 Core 的 `CommandBuffer`（双缓冲），**不在调用线程即时执行**，
而是由 `GCore_BeginFrame(dt)` 在帧边界统一消费（先 `swap()` 再逐条 `process_command`），
Play 模式下随后执行 `world->update(dt)`。队列满时丢弃并计数，可经
`GCore_GetCmdQueueCapacity` / `GCore_GetDroppedCmdCount` 监控。

**payload 布局约定**（C# 与 C++ 双方手写对齐，如 `CREATE_ENTITY` = `char name[128] + GEntityHandle parent`、
`SET_PROPERTY` = `GEntityHandle h + uint64_t type_hash + char prop_name[64] + uint8_t value[128]`）。

### 3.4 回调协议

编辑器通过 `GCore_RegisterCallback_*` 注册函数指针（C# 侧 delegate 须存字段防 GC）：

| 回调 | 触发时机 |
|---|---|
| `OnEntityListChanged` | 实体增删/改名/换父后 |
| `OnEntitySelected` / `OnEntityDeselected` | 选中/取消选中 |
| `OnSceneLoaded` | 场景加载完成 |
| `OnPlayModeChanged` | Play/Stop/Pause 状态变化 |
| `OnLogMessage` | 引擎日志（`MemoryLogSink` 增量转发） |
| `OnComponentChanged` | 组件字段被 `SetProperty` 修改后 |

回调**非即时**：命令执行时只置 `deferred_*` 标志，`GCore_EndFrame()` 统一触发，
并顺带 `drain_log_messages()` 推送日志。编辑器侧再 `Dispatcher.Invoke` 回 UI 线程刷新
Hierarchy / Inspector / Console。

### 3.5 句柄模型

`GEntityHandle`（int）是编辑器对实体的唯一引用。Core 内部 `EntityHandleMap`
维护 handle ↔ UUID 双向映射，`EntityResolver::resolve(h)` 先查 UUID 再在场景中
`find_entity_by_uuid`。场景重载后 map 重建，句柄不再有效但绝不悬垂。

`GCore_GetInternalWorldPtr()` 返回 `World*`，**仅供同进程其他 DLL 使用**
（GrycePhysics 经 `GPhysics_AttachSystems` 把物理系统注册进 World），
不作为编辑器公共接口。

### 3.6 编辑器接入流程

```csharp
// EngineService.Initialize(projectRoot)
GCore_Init(ref desc);                          // desc.ProjectRoot 作为 res:/ 根
GPhysics_Init(GPhysicsBackend.Jolt);           // 创建 3D/2D 物理世界
GPhysics_AttachSystems(GCore_GetInternalWorldPtr()); // 物理系统挂入 World

// EditorViewModel 构造：注册回调
GCore_RegisterCallback_OnEntityListChanged(_onEntityListChanged); // 等 7 个

// 每帧（UI 线程 60Hz DispatcherTimer）
GCore_BeginFrame(dt);
GCore_EndFrame();

// 渲染（专用线程）：见 ARCHITECTURE.md §14.4
GRender_BeginFrame(); GRender_RenderWorld(); GRender_RenderGizmo(); GRender_EndFrame();
```

---

## 4. 内部 C++ API（引擎扩展开发）

> 以下章节描述引擎内部 C++ 接口（编译进各 DLL 的 `gryce_engine` 命名空间）。
> 编辑器与外部工具**不直接使用**这些接口，只通过第 3 节的 C API 通信。

所有公共 C++ API 位于 `gryce_engine` 命名空间及其子命名空间下：

| 命名空间 | 内容 |
|---|---|
| `gryce_engine::scene` | Scene、Entity、UUID、SceneSerializer、Prefab |
| `gryce_engine::ecs` | World、ISystem、ComponentStore、EntityID |
| `gryce_engine::components` | Component 基类、Transform、所有具体组件 |
| `gryce_engine::render` | IRenderBackend、RenderContext、IRenderer2D、Material |
| `gryce_engine::math` | Vector2f/3f/4f、Matrix4f、Quaternionf、Camera |
| `gryce_engine::platform` | Window、Input、Cursor |
| `gryce_engine::assets` | AssetManager、MeshData、TextureData、AssetHandle、AsyncLoader |
| `gryce_engine::resources` | ResourcePath、Project、Tileset |
| `gryce_engine::physics` | IPhysicsWorld2D/3D、PhysicsFactory、PhysicsTypes |
| `gryce_engine::animation` | Skeleton、AnimationClip、Pose |
| `gryce_engine::reflection` | 组件反射注册与字段访问 |
| `gryce_engine::utils` | FrameLimiter、glog |

---

## 5. 内部公共头文件清单

以下头文件为引擎内部 C++ 接口（编译进各 DLL），供 C++ 扩展开发通过
`#include "<path>"` 引用：

### 场景与 ECS

```
scene/entity.h
scene/scene.h                  # Scene；每个场景有且仅有一个合成根 Entity（Scene::root()），.gesc 版本 2（v1 兼容加载）
scene/prefab.h               # Prefab 加载与实例化
scene/uuid.h
ecs/world.h
ecs/system.h
ecs/types.h
ecs/query.h
ecs/component_store.h
animation/skeleton.h
animation/animation_clip.h
animation/pose.h
```

### 组件

```
components/component.h
components/component_factory.h
components/transform.h
components/node3d.h
components/node2d.h
components/camera.h
components/light.h
components/mesh_renderer.h
components/skinned_mesh_renderer.h
components/terrain.h
components/prefab_instance.h
components/audio_listener.h
components/audio_source.h
components/physics_body.h
components/static_body.h
components/rigid_body.h
components/box_collider.h
components/sphere_collider.h
components/plane_collider.h
components/physical_material.h
components/destructible_body.h
components/fragment_body.h
components/static_body_2d.h
components/rigid_body_2d.h
components/box_collider_2d.h
components/circle_collider_2d.h
components/character_controller_3d.h
components/character_controller_2d.h
components/joint_3d.h
components/joint_2d.h
components/2d/component_2d.h   # 2D 组件基类；world_transform_2d() 父链变换（Node2D::top_level 脱离，z_index 参与排序）
components/2d/basic_rect.h
components/2d/shape.h
components/2d/label.h
components/2d/light_2d.h
components/2d/ambient_light_2d.h
components/2d/skybox_2d.h
components/2d/sprite_2d.h
components/2d/tilemap.h
components/2d/camera_2d.h
components/2d/parallax_background.h
components/2d/particle_emitter.h
```

### 渲染

```
render/render.h              # IRenderBackend、RenderAPI 枚举（Vulkan 默认 / OpenGL 兼容 / DX11、DX12 预留）、create_render_backend()
render/render2d.h            # IRenderer2D
render/render_context.h      # RenderContext
render/render_pipeline.h     # RenderPipeline
render/render_command_buffer.h
render/render_thread.h
render/mesh.h
render/shader.h
render/texture.h
render/material.h
render/framebuffer.h
render/font_atlas.h
render/renderer2d_impl.h
render/imgui_backend.h       # IImGuiBackend
render/rhi_handle.h          # RHIMeshHandle 等
render/opengl/gl_backend.h   # GLBackend（OpenGL 专用）
render/opengl/gl_buffer.h
render/opengl/gl_shader.h
render/opengl/gl_texture.h
render/opengl/gl_framebuffer.h
render/opengl/gl_utils.h
render/opengl/gl_imgui_backend.h
render/opengl/imgui_renderer.h
# Vulkan 头文件仅在 GRYCE_HAS_VULKAN 时可用
```

### 数学

```
math/math.h                  # Vector2f/3f/4f, Matrix4f, Quaternionf
math/camera.h
```

### 平台

```
platform/window.h
platform/input.h
platform/cursor.h
```

### 资源与资产

```
assets/asset.h
assets/asset_handle.h
assets/asset_manager.h
assets/async_loader.h
assets/obj_loader.h
assets/mesh_data.h
assets/texture_data.h
resources/project.h
resources/resource_path.h
resources/tileset.h
```

### 物理

```
physics/physics_point.h
physics/physics_types.h
physics/physics_world_2d.h
physics/physics_world_3d.h
physics/physics_factory.h
physics/box2d_world_2d.h
physics/jolt_world_3d.h       # Jolt Physics 后端（GRYCE_HAS_JOLT）
```

### 工具

```
utils/frame_limiter.h
utils/glog/glog_lib.h          # GLog + AsyncLogger（log() 入队、worker 线程写出；GLog 自动包装 logger；flush() 等待排空）
export.h                     # GRYCE_API 宏
```

---

## 6. API 使用规范

### 5.1 Entity 与 Component

```cpp
#include "scene/entity.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"

using namespace gryce_engine;

// 创建 Entity
auto entity = std::make_unique<scene::Entity>("Cube");

// 添加组件（Entity 自动拥有 Transform）
auto* mesh = entity->add_component<components::MeshRenderer>();
mesh->set_mesh_path("res:/models/cube_pbr.obj");

// 访问 Transform
entity->transform()->position = math::Vector3f(1.0f, 2.0f, 3.0f);
```

> **注意**：每个 `Scene` 有且仅有一个合成根 Entity（`Scene::root()`）；手工创建的顶层 Entity 应作为根的子节点加入场景。`.gesc` 序列化格式为版本 2（v1 文件可原样加载）。

### 5.2 World 与 System

```cpp
#include "ecs/world.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_3d.h"

using namespace gryce_engine;

auto world = std::make_unique<ecs::World>();

// 加载场景
auto scene = scene::Scene::load("res:/scenes/main.gesc");
world->attach_scene(std::move(scene));

// 注册系统
world->add_system<ecs::PhysicsSystem3D>();
world->add_system<ecs::RenderSystem3D>();

// 初始化
world->init();

// 主循环
while (!window.should_close()) {
    float dt = window.delta_time();
    world->update(dt);
    // render 阶段由 RenderSystem 内部通过 RenderContext 提交
}

world->shutdown();
```

### 5.3 渲染后端

```cpp
#include "render/render.h"
#include "platform/window.h"

using namespace gryce_engine;

// 创建窗口（OpenGL 上下文）
platform::Window window("My Game", 1280, 720, platform::WindowMode::Windowed);

// 创建渲染后端：RenderAPI { Vulkan（默认）, OpenGL（兼容）, DX11 / DX12（预留，返回 nullptr） }
auto backend = render::create_render_backend(render::RenderAPI::Vulkan);
backend->init(window.native_handle());

// 帧循环
while (!window.should_close()) {
    backend->begin_frame();
    backend->clear(0.1f, 0.1f, 0.1f, 1.0f);
    
    // 绘制命令...
    
    backend->end_frame();
    window.swap_buffers();
}

backend->shutdown();
```

### 5.4 资源路径

```cpp
#include "resources/resource_path.h"

// 解析 res:/ 路径
std::string real_path = gryce_engine::resources::resolve_path("res:/textures/cursor.png");
// 返回 <project_root>/textures/cursor.png
```

### 5.5 材质与 3D 渲染管线

```cpp
#include "render/material.h"
#include "render/render_pipeline.h"

using namespace gryce_engine;

// ---- Material ----
render::Material mat;
mat.albedo_color = math::Vector3f(0.8f, 0.2f, 0.2f);
mat.roughness = 0.4f;
mat.metallic = 0.9f;
mat.emissive_color = math::Vector3f(4.0f, 1.6f, 0.3f); // HDR，可 >1
mat.opacity = 0.35f;
mat.blend_mode = render::Material::BlendMode::Blend;   // Opaque(默认)/Blend
mat.two_sided = true;
mat.uv_scale = math::Vector2f(3.0f, 3.0f);
mat.save_to_file("res:/materials/my.gmat");            // JSON 材质资源
mat.load_from_file("res:/materials/my.gmat");

// ---- RenderPipeline ----
render::RenderPipeline pipeline;
pipeline.init(&render_ctx, "res:/shaders");
pipeline.set_skybox({ "res:/textures/skybox/px.png", "res:/textures/skybox/nx.png",
                      "res:/textures/skybox/py.png", "res:/textures/skybox/ny.png",
                      "res:/textures/skybox/pz.png", "res:/textures/skybox/nz.png" }); // 须在 RenderContext::start() 前

std::vector<render::RenderPipeline::Light> lights;
render::RenderPipeline::Light sun;                     // 默认 Directional
sun.direction = math::Vector3f(-0.6f, -0.7f, 0.0f);
sun.intensity = 3.0f;
lights.push_back(sun);
render::RenderPipeline::Light lamp;
lamp.type = render::RenderPipeline::LightType::Point;  // Directional/Point/Spot
lamp.position = math::Vector3f(0.0f, 4.0f, 0.0f);
lamp.range = 30.0f;
lights.push_back(lamp);                                // 最多 k_max_lights=8 盏

pipeline.set_lights(lights);
pipeline.set_ambient(math::Vector3f(0.15f, 0.15f, 0.15f));
pipeline.set_shadow_enabled(true);
pipeline.set_exposure(1.0f);
pipeline.set_tone_map_mode(1);                         // 0=None, 1=Reinhard, 2=ACES

pipeline.set_camera(camera);
pipeline.render_scene(scene, render_ctx);              // shadow -> skybox -> 不透明 -> 透明 -> tonemap
```

---

## 7. 扩展规范

### 6.1 自定义组件

自定义组件须继承 `components::Component` 并实现以下虚函数：

```cpp
class MyComponent : public components::Component {
public:
    const char* type() const override { return "MyComponent"; }
    
    void serialize(nlohmann::json& out) const override {
        out["speed"] = speed;
    }
    
    void deserialize(const nlohmann::json& in) override {
        speed = in.value("speed", 1.0f);
    }
    
    void on_update(float dt) override {
        // 每帧逻辑
    }
    
    float speed = 1.0f;
};
```

组件须在 `component_factory.cpp` 中注册，否则场景序列化时无法反序列化：

```cpp
ComponentFactory::register_type("MyComponent", []() {
    return std::make_unique<MyComponent>();
});
```

### 6.2 自定义系统

自定义系统须继承 `ecs::ISystem`：

```cpp
class MySystem : public ecs::ISystem {
public:
    void on_update(scene::Scene& scene, float dt) override {
        for (auto& entity : scene.entities()) {
            if (auto* comp = entity->get_component<MyComponent>()) {
                comp->speed += dt;
            }
        }
    }
};
```

系统在 `World` 中的注册顺序决定执行顺序。

### 6.3 自定义渲染后端

自定义后端须实现 `render::IRenderBackend`：

```cpp
class MyBackend : public render::IRenderBackend {
public:
    bool init(void* native_window) override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    // ... 实现所有纯虚函数
};
```

注册方式：

```cpp
// 在引擎初始化前注册
render::register_backend("myapi", []() { return std::make_unique<MyBackend>(); });
```

---

## 8. 线程安全

所有导出的 **C API 入口**（`GCore_*` / `GEntity_*` / `GScene_*` / `GComponent_*` /
`GRender_*` / `GWindow_*` / `GPhysics_*` 等，四个 DLL 内约 110 个函数）在内部统一持有
同一把递归互斥锁（`GRYCE_API_GUARD()`，见 `core/GryceCore/api_guard.h`），因此编辑器
的 UI 线程（60Hz tick、输入、Hierarchy/Inspector 读取）与专用渲染线程（vsync 240Hz）
可以并发安全地调用 C API；递归锁允许 API 互调及回调在持锁线程上重入。

注意：`GRender_EndFrame` 在锁内执行渲染命令，但 `glfwSwapBuffers`（present_swap）
在**锁外**进行，因此 vsync 停滞最多让渲染线程等待，不会冻结编辑器 UI 线程。

| 接口 | 线程安全 |
|---|---|
| `World::update()` | 主线程 |
| `Entity::add_component()` | 主线程（初始化阶段） |
| `RenderContext::push_command()` | 主线程 |
| `IRenderBackend::*` | 渲染线程（通过命令队列间接调用） |
| `AssetManager::load_*()` | 主线程（内部有异步加载，返回前同步等待） |

---

## 9. 编译宏

| 宏 | 条件 | 说明 |
|---|---|---|
| `GRYCE_HAS_VULKAN` | Vulkan SDK 可用 | 暴露 Vulkan 后端头文件与实现 |
| `GRYCE_HAS_IMGUI` | ImGui 源可用 | 编译 ImGui 后端集成 |
| `GRYCE_HAS_BOX2D` | Box2D 可用 | 启用 Box2D 物理后端 |
| `GRYCE_HAS_JOLT` | Jolt Physics 可用 | 启用 Jolt 物理后端 |
| `GRYCE_HAS_ASSIMP` | Assimp 可用 | 启用 Assimp 模型导入 |
| `GRYCE_CORE_BUILDING` 等 | DLL 构建方 | 各 DLL 的导出宏定义（`GryceCore` 编译时定义 `GRYCE_CORE_BUILDING` 等，使 `*_API` 展开为 `dllexport`） |

---

## 10. 依赖清单

构建引擎 DLL 所需的外部依赖：

| 依赖 | 用途 | 是否必须 |
|---|---|---|
| GLFW | 窗口、输入 | 是 |
| GLEW | OpenGL 加载器 | 是 |
| Vulkan SDK | Vulkan 后端 | 否（无则禁用 Vulkan） |
| nlohmann/json | 场景序列化 | 是 |
| ImGui | 调试 UI | 是（随仓库 vendored） |
| stb_image / stb_truetype | 纹理/字体加载 | 是（随仓库 vendored） |
| miniaudio | 音频 | 是（随仓库 vendored） |
| Box2D | 2D 物理 | 否 |
| Jolt Physics | 3D 物理 | 否 |
| Assimp | 模型导入 | 否 |
