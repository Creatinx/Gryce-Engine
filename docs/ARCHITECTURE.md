# Gryce Engine Architecture

> 本文档从架构、模块、数据流三个维度对 Gryce Engine 进行系统性描述。

---

## 1. 项目概况

| 项 | 值 |
|---|---|
| 名称 | Gryce Engine |
| 版本 | 0.1.0 |
| 语言 | C++23 |
| 构建 | CMake + Ninja |
| 平台 | Windows（MSYS2 UCRT64 / MinGW-w64，MSVC 2022+）|
| 渲染后端 | OpenGL 4.6、Vulkan 1.2 |
| 架构风格 | ECS + 场景树混合 |

---

## 2. 目录结构

```
Gryce-Engine/
├── cmake/                  # 编译器选项、依赖 FetchContent 脚本
├── core/                   # 引擎核心静态库（gryce_core）
│   ├── assets/             # 资源加载器
│   ├── audio/              # 音频系统
│   ├── components/         # ECS 组件定义
│   │   └── 2d/             # 2D 专用组件
│   ├── ecs/                # ECS 调度与系统
│   │   └── systems/        # 各系统实现
│   ├── math/               # 数学库
│   ├── physics/            # 物理抽象与实现
│   ├── platform/           # 平台抽象（窗口、输入、光标）
│   ├── render/             # 渲染抽象 + OpenGL/Vulkan 实现
│   │   ├── opengl/         # GL 后端实现
│   │   └── vulkan/         # Vulkan 后端实现
│   ├── resources/          # res:/ 路径解析、项目根
│   ├── scene/              # Scene、Entity、Transform、UUID
│   └── utils/              # 日志、帧率限制
├── docs/                   # 文档
├── editor/                 # 编辑器入口
├── examples/               # 示例游戏项目
│   ├── common/             # 示例公共框架（app_launcher、debug_panel）
│   ├── 3dtest/             # 3D 综合演示项目
│   ├── gt2dDemo/           # 2D 综合演示项目
│   ├── demo_sprite2d/      # 2D Sprite2D 演示
│   ├── demo_shapes2d/      # 2D 形状演示
│   ├── demo_lighting2d/    # 2D 光照演示
│   ├── demo_tilemap2d/     # 2D 瓦片地图演示
│   ├── demo_particles2d/   # 2D 粒子演示
│   ├── demo_physics2d/     # 2D 物理演示
│   ├── demo_character2d/   # 2D 角色控制器演示
│   ├── demo_joints2d/      # 2D 关节演示
│   ├── demo_physics3d/     # 3D 物理演示
│   ├── demo_character3d/   # 3D 角色控制器演示
│   ├── demo_joints3d/      # 3D 关节演示
│   ├── demo_fracture/      # 3D 碎裂演示
│   ├── demo_lighting3d/    # 3D 光照演示
│   ├── demo_audio3d/       # 3D 音频演示
│   └── demo_scene_serializer/ # 场景序列化演示
├── tests/                  # 单元测试
└── third_party/            # ImGui、nlohmann/json、stb、miniaudio
```

---

## 3. 构建系统

### 3.1 CMake 结构

- 根 `CMakeLists.txt`：项目定义、C++23 标准、输出目录、子目录。
- `cmake/compiler_options.cmake`：编译警告、Debug/Release 优化选项。
- `cmake/dependencies.cmake`：第三方依赖（GLFW、GLEW、Vulkan、GTest 等）。
- `core/CMakeLists.txt`：核心库源文件与链接依赖。
- `editor/CMakeLists.txt`：编辑器可执行文件。
- `examples/CMakeLists.txt`：示例程序。
- `tests/CMakeLists.txt`：单元测试。

### 3.2 输出目录

```cmake
CMAKE_RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR}/bin
CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG = ${CMAKE_BINARY_DIR}/bin/Debug
CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE = ${CMAKE_BINARY_DIR}/bin/Release
```

### 3.3 运行时 DLL 复制

MinGW 下构建后自动复制：
- `libgcc_s_seh-1.dll`
- `libstdc++-6.dll`
- `libwinpthread-1.dll`
- `glew32.dll`
- `glfw3.dll`

### 3.4 目标产物

| 目标 | 类型 | 说明 |
|---|---|---|
| `gryce_core` | 静态库 | 引擎核心（默认，可通过 `GRYCE_BUILD_SHARED=ON` 切换为动态库） |
| `gryce_editor` | 可执行文件 | 编辑器 |
| `examples_common` | 静态库 | 示例公共框架（app_launcher、debug_panel） |
| `3dtest` | 可执行文件 | 3D 综合演示（物理、碎裂、光照、关节、角色控制器、场景保存） |
| `gt2dDemo` | 可执行文件 | 2D 综合演示（平台跑酷、光照、粒子、瓦片地图、关节桥、形状） |
| `demo_sprite2d` | 可执行文件 | 2D Sprite2D 演示 |
| `demo_shapes2d` | 可执行文件 | 2D 形状演示 |
| `demo_lighting2d` | 可执行文件 | 2D 光照演示 |
| `demo_tilemap2d` | 可执行文件 | 2D 瓦片地图演示 |
| `demo_particles2d` | 可执行文件 | 2D 粒子演示 |
| `demo_physics2d` | 可执行文件 | 2D 物理演示 |
| `demo_character2d` | 可执行文件 | 2D 角色控制器演示 |
| `demo_joints2d` | 可执行文件 | 2D 关节演示 |
| `demo_physics3d` | 可执行文件 | 3D 物理演示 |
| `demo_character3d` | 可执行文件 | 3D 角色控制器演示 |
| `demo_joints3d` | 可执行文件 | 3D 关节演示 |
| `demo_fracture` | 可执行文件 | 3D 碎裂演示 |
| `demo_lighting3d` | 可执行文件 | 3D 光照演示 |
| `demo_audio3d` | 可执行文件 | 3D 音频演示 |
| `demo_scene_serializer` | 可执行文件 | 场景序列化演示 |
| `gryce_tests` | 可执行文件 | 单元测试（GTest） |

---

## 4. 渲染架构（RHI）

### 4.1 设计原则

- **接口抽象**：上层逻辑通过 `IRenderBackend` 等接口与具体图形 API 解耦。
- **命令队列**：主线程把渲染指令压入 `RenderCommandBuffer`，渲染线程消费执行。
- **资源延迟销毁**：GPU 资源在对应帧渲染完成后才释放，避免跨线程 use-after-free。
- **RHI 句柄化**：资源通过 `RHIMeshHandle`、`RHITextureHandle` 等句柄引用，替代裸指针。

### 4.2 核心接口

```
IRenderBackend
├── create_mesh()       → RHIMeshHandle
├── create_texture()    → RHITextureHandle
├── create_shader()     → RHIShaderHandle
├── create_framebuffer()→ RHIFramebufferHandle
├── draw_mesh()         # 使用 RHI 句柄绘制
├── create_renderer2d() → IRenderer2D
├── create_imgui_backend() → IImGuiBackend
├── begin_frame()
├── end_frame()
└── flush_gpu() / wait_gpu_idle()
```

### 4.3 OpenGL 后端

- `GLBackend`：管理 GL 上下文、ImGui 后端、状态切换。
- `GLMesh`：VAO/VBO/IBO 封装。
- `GLTexture`：2D 纹理上传与绑定。
- `GLShader`：着色器编译、uniform 设置、多 render pass 管线。
- `GLFramebuffer`：FBO 用于 shadow map、HDR、后处理。
- `GLRenderer2D`：2D 批处理渲染（矩形、文字、精灵、形状）。
- `GLRenderer3D`：3D 网格渲染。
- `GLFramePacing`：帧率限制与 NVIDIA `WGL_NV_delay_before_swap`。

### 4.4 Vulkan 后端

- `VulkanInstance`：实例创建、验证层、调试 messenger。
- `VulkanDevice`：物理设备选择、逻辑设备、队列、描述符池。
- `VulkanSwapchain`：交换链、depth attachment、帧缓冲。
- `VulkanMesh` / `VulkanTexture` / `VulkanShader` / `VulkanFramebuffer`：对应 GPU 资源。
- `VulkanRenderer2D`：Vulkan 2D 批处理。
- `VulkanImGuiBackend`：ImGui Vulkan 后端。

### 4.5 渲染管线（3D）

```
1. Shadow Pass        → 渲染 depth map（2048x2048）
2. Main Pass          → PBR 着色 + shadow sampling
3. HDR Post-Process   → tone mapping
4. 2D Overlay         → UI、文字、DebugPanel
5. Present            → 交换到屏幕
```

---

## 5. ECS 架构

### 5.1 核心概念

Gryce 采用 ECS + 场景树混合方案：
- `Entity` 是场景中的节点，带 `Transform` 和父子关系。
- `Component` 挂载在 `Entity` 上，负责数据与局部逻辑。
- `ISystem` 遍历场景中的组件，执行全局更新。

### 5.2 关键类

| 类 | 职责 |
|---|---|
| `scene::Scene` | 管理所有 Entity，序列化/反序列化 `.gesc`。 |
| `scene::Entity` | 节点，拥有 Transform、Component 列表、父子引用。 |
| `components::Component` | 组件基类，提供 `type()`、`serialize()`、`deserialize()`、`on_update()`。 |
| `ecs::ISystem` | 系统基类，提供 `on_update(scene, dt)`。 |
| `ecs::World` | 持有 Scene 和 Systems，负责主循环调度。 |
| `components::ComponentFactory` | 通过类型名反射创建组件。 |
| `scene::Prefab` | 预制体模板，从 `.gesc` 加载并实例化到任意 Scene。 |

### 5.3 生命周期

```
Scene::load() / create_entity()
    → Entity::add_component<T>()
        → Component::on_attach() / 默认值
        → Component::on_awake()

World::init()
    → Scene::init()
        → Entity::on_init()      // 递归初始化所有组件
        → Entity::on_start()     // 场景正式开始

World::update(dt)
    → 各 System::on_update(scene, dt)  // 按 phase + priority 排序
        → Entity::on_update(dt)
            → Component::on_update(dt)
```

当前组件生命周期包含 `on_attach`、`on_awake`、`on_init`、`on_start`、`on_enable`、`on_disable`、`on_update`、`on_render`、`on_destroy`。

### 5.4 已实现的系统

| 系统 | 说明 |
|---|---|
| `PhysicsSystem3D` | 3D 物理积分、碰撞检测、睡眠、角色控制器、关节。 |
| `PhysicsSystem2D` | 2D 物理、碰撞检测、角色控制器、关节。 |
| `FractureSystem` | 检测 `DestructibleBody` 冲量并生成碎片。 |
| `RenderSystem2D` | 收集 2D 组件并提交到渲染器。 |
| `RenderSystem3D` | 收集 3D `MeshRenderer` 并提交。 |

---

## 6. 场景系统

### 6.1 `.gesc` 格式

JSON 结构：

```json
{
  "name": "main",
  "version": 1,
  "entities": [
    {
      "name": "Cube",
      "uuid": "...",
      "parent": null,
      "transform": { "position": [...], "rotation": [...], "scale": [...] },
      "components": [
        { "type": "MeshRenderer", "mesh_path": "res:/models/cube_pbr.obj", "material": {...} },
        { "type": "RigidBody", "mass": 1.0 },
        { "type": "BoxCollider", "size": [...] }
      ]
    }
  ]
}
```

### 6.2 虚拟路径 `res:/`

- `res:/scenes/main.gesc` → 项目根目录下的 `scenes/main.gesc`。
- 解析逻辑在 `core/resources/resource_path.cpp`。
- 运行示例时通过 `--project-root <path>` 指定项目根。

### 6.3 序列化机制

每个组件实现：
- `serialize(json& out)`：写入 JSON。
- `deserialize(const json& in)`：读取 JSON。

`ComponentFactory` 根据 `"type"` 字段创建对应组件实例。

### 6.4 预制体（Prefab）

Prefab 是场景的可复用模板：

```
Prefab::load("res:/prefabs/enemy_tank.gesc")
    → 加载为独立的 Entity 树（不关联 ComponentStore）
    → Prefab 持有根实体列表

Prefab::instantiate(scene)
    → Entity::clone() 深拷贝每个实体
        → 新 UUID、新 EntityID
        → 组件通过 serialize/deserialize 深拷贝
    → scene->add_root_entity(cloned)
```

使用方式：

```cpp
auto prefab = scene::Prefab::load("res:/prefabs/coin.gesc");
for (int i = 0; i < 10; ++i) {
    auto* coin = prefab->instantiate(scene);
    coin->transform()->position = math::Vector3f(i * 2.0f, 0, 0);
}
```

---

## 7. 组件系统

### 7.1 变换与层级

- `Transform`：统一 2D/3D 变换，使用 `Vector3f` + `Quaternionf`。
- `Entity::parent()` / `children()`：维护父子关系。
- 世界矩阵通过递归计算：`world = parent_world * local`。

### 7.2 3D 渲染组件

| 组件 | 说明 |
|---|---|
| `MeshRenderer` | 网格路径 + Material，负责异步上传到 GPU。 |
| `Camera` | FOV、near/far、is_main。 |
| `Light` | light_type（directional/point/spot）、color、intensity、range、spot_angle。 |

### 7.3 物理组件

| 组件 | 说明 |
|---|---|
| `RigidBody` | 动态刚体：mass、velocity、acceleration、restitution、friction、damping。 |
| `StaticBody` | 静态碰撞体。 |
| `BoxCollider` / `SphereCollider` / `PlaneCollider` | 3D 碰撞形状。 |
| `RigidBody2D` / `StaticBody2D` / `BoxCollider2D` / `CircleCollider2D` | 2D 刚体与碰撞形状。 |
| `CharacterController3D` / `CharacterController2D` | 平台角色控制器：移动、跳跃、坡度限制、台阶抬升。 |
| `Joint3D` / `Joint2D` | 关节组件：Fixed、Hinge、Distance、Spring。 |
| `PhysicalMaterial` | 材质预设（Metal、Concrete、Wood 等）：softness、drag、density。 |
| `DestructibleBody` | 碎裂配置：threshold、impulse、segments、max_fragments、lifetime。 |
| `FragmentBody` | 碎片生命周期管理。 |

### 7.4 2D 渲染组件

| 组件 | 说明 |
|---|---|
| `ColorRect` | 纯色矩形。 |
| `Label` | TTF 文字渲染。 |
| `Sprite2D` | 2D 精灵贴图。 |
| `Circle` / `Polygon` | 2D 形状。 |
| `TileMap` | 瓦片地图。 |
| `ParticleEmitter2D` | 2D 粒子发射器。 |
| `ParallaxBackground` | 视差背景。 |
| `Light2D` | 2D 光源组件（占位）。 |

---

## 8. 物理系统

### 8.1 当前实现

物理后端已全面切换到成熟第三方库：

| 维度 | 后端 | 状态 |
|---|---|---|
| 2D | Box2D v3.0.0 | 已接入，默认启用 |
| 3D | Jolt Physics v5.2.0 | 已接入，默认启用 |
| 自研 | BuiltinPhysicsWorld2D/3D | 已删除 |

- **碰撞检测**：由 Box2D / Jolt 的 broadphase + narrowphase 完成。
- **积分**：Box2D 使用子步求解器；Jolt 使用 `PhysicsSystem::Update` 内部积分。
- **睡眠**：依赖底层引擎的睡眠机制，通过 `is_sleeping` 字段暴露给上层。
- **碎裂**：`FractureSystem` 在检测到碰撞冲量超过 `DestructibleBody::threshold` 时，按网格切分生成 `FragmentBody` 并交给 Jolt/Box2D 继续模拟。

### 8.2 设计限制

- 仍缺少连续碰撞检测（CCD）配置，高速小物体可能穿透。
- 通用 mesh 碎裂（Voronoi / 有限元）未实现，当前只支持立方体网格切分。
- 物理材质仅通过 `PhysicalMaterial` 组件映射到摩擦/弹性/密度，更复杂的表面属性尚未支持。
- 关节系统目前仅支持 Box2D/Jolt 原生类型（Fixed/Hinge/Distance/Spring），部分后端约束类型尚未完全映射。

### 8.3 物理后端抽象

引擎提供统一的物理接口：
- `IPhysicsWorld2D` / `IPhysicsWorld3D`：物理世界抽象。
- `Box2DPhysicsWorld2D`：Box2D v3 封装（默认 2D 后端，需要 `GRYCE_HAS_BOX2D=ON`）。
- `JoltPhysicsWorld3D`：Jolt Physics 封装（默认 3D 后端，需要 `GRYCE_HAS_JOLT=ON`）。
- `PhysicsFactory` 通过字符串 `"box2d"` / `"jolt"` 创建对应后端。

---

## 9. 资源管线

### 9.1 AssetManager

- 缓存 mesh、texture、material。
- 路径作为 key，避免重复加载。
- 提供 `load_mesh()`、`load_texture()`、`get_material()` 等接口。

### 9.2 模型加载

- `ObjLoader`：解析 `.obj` + `.mtl`（部分）。
- `AssimpImporter`：通过 Assimp v5.4.3 加载 OBJ、FBX、gITF、DAE、PLY、STL 等常见格式。
- 输出 `MeshData`：顶点位置、法线、切线、UV、索引。
- `MeshData::to_physics_points()` 可将顶点转换为物理质点，方便后续软体/碎裂扩展。

### 9.3 纹理加载

- `stb_image` 加载 PNG/JPG/BMP。
- 支持 1/3/4 通道，自动上传到 GPU。
- FontAtlas 用 `stb_truetype` 生成 512x512 或更大图集。

### 9.4 材质

- `Material` 结构体：albedo_color、roughness、metallic、ao、各贴图路径与 use_xxx_map 开关。
- MeshRenderer 上传时同时上传材质到 GPU（OpenGL UBO / Vulkan uniform buffer + descriptor set）。
- 材质资源文件 `.gmat` 尚未实现。

---

## 10. 输入与平台

### 10.1 Window

- GLFW 抽象：`platform::Window` 类。
- 支持窗口化、无边框、VSync、大小调整。
- 焦点管理：只有窗口有焦点时才处理鼠标锁定与输入。

### 10.2 Input

- 键盘：`is_key_pressed`、`is_key_held`、`is_key_released`。
- 鼠标：位置、delta、按键状态。
- 鼠标锁定模式：用于 FPS 视角控制。

### 10.3 Cursor

- 自定义光标贴图支持。
- 鼠标锁定时隐藏光标（`glfwSetInputMode(GLFW_CURSOR_DISABLED)`）。
- 无焦点时恢复系统默认光标。

---

## 11. UI 系统

### 11.1 ImGui 层

- `ImGuiRenderer`：初始化 ImGui + GLFW + 后端。
- `GLImGuiBackend` / `VulkanImGuiBackend`：具体后端实现。
- `DebugPanel`：共享调试面板，用于 FPS、帧率限制、输入切换、物理材质显示。

### 11.2 运行时 2D UI

- 通过 ECS 组件实现：`ColorRect`、`Label`、`Sprite2D` 等。
- `RenderSystem2D` 收集并排序（`render_order`）后提交。
- 坐标系：屏幕左上角为原点，X 向右，Y 向下。

---

## 12. 多线程模型

### 12.1 渲染线程

```
主线程
  ├── 逻辑更新（ECS Systems）
  ├── 收集渲染命令
  └── push_command(lambda) ──────────────┐
                                         ▼
                               RenderCommandBuffer
                                         ▼
                                渲染线程（独立）
                                    执行命令
                                         ▼
                                     GPU / Swap
```

### 12.2 同步机制

- `RenderCommandBuffer`：双缓冲/多缓冲命令队列。
- `pending_destroys_`：帧延迟销毁队列，资源在 `safe_seq` 帧完成后才释放。
- `MeshRenderer::uploaded_`：原子标志，避免主线程在材质上传完成前渲染。

### 12.3 线程安全约束

- 渲染 lambda 必须捕获值或安全对象，避免悬空引用。
- GPU 资源释放必须通过 `RenderContext::destroy_*` 走延迟队列。

---

## 13. 关键数据流

### 13.1 一帧的主循环

```
Window::poll_events()
Input::update()
ImGui::NewFrame()

World::update(dt)
    PhysicsSystem::on_update(scene, dt)
    FractureSystem::on_update(scene, dt)
    ...其他系统...

CameraSystem / RenderSystem 收集相机、灯光、网格、2D UI
    RenderContext::push_command(lambda) 提交渲染命令

RenderThread 执行命令
    Shadow pass → Main pass → HDR → 2D overlay → present

Window::swap_buffers() / backend::present()
```

### 13.2 资源加载到渲染

```
MeshRenderer::set_mesh_path("res:/models/cube_pbr.obj")
    AssetManager::load_mesh(path)
        ObjLoader::load(file_path)
    upload_to_gpu()
        创建 GPU mesh + 上传 material 纹理
        uploaded_ = true

RenderSystem3D 收集 MeshRenderer
    if (uploaded_) draw_mesh(gpu_mesh, material, transform)
```

### 13.3 场景保存

```
Scene::serialize()
    遍历所有 Entity
        Entity::serialize()
            Transform::serialize()
            遍历 Components
                Component::serialize()
    写入 JSON 文件 res:/scenes/main.gesc
```

---

## 14. 扩展点

### 14.1 添加新组件

1. 在 `core/components/`（或 `core/components/2d/`）新建头文件。
2. 继承 `Component`，实现 `type()`、`serialize()`、`deserialize()`、`on_update()`（可选）。
3. 在 `core/components/component_factory.cpp` 注册类型。
4. 在对应 System 中处理该组件。

### 14.2 添加新渲染后端

1. 实现 `IRenderBackend`、`IShader`、`ITexture`、`IMesh`、`IFramebuffer`、`IRenderer2D`、`IRenderer3D`。
2. 在 `RenderContext` 中注册后端创建函数。
3. 添加命令行参数切换后端。

### 14.3 添加新系统

1. 继承 `ecs::ISystem`。
2. 在 `World` 构造或初始化时注册。
3. 注意 System 执行顺序依赖。
