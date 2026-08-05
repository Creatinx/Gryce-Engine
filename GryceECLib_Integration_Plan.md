# GryceECLib 接入现有引擎方案

> 目标：把现有 Gryce Engine 的 Core 能力（ECS、渲染、物理、输入）封装成独立的 `GryceECLib.dll`，对外暴露纯 C API，供 WPF 编辑器通过 P/Invoke 调用。

## 1. 总体架构

不再只生成一个 DLL，而是按命名空间拆成多个模块，最后由 `GryceECLib.dll` 作为统一的 C API 门面：

```
WPF Editor (C#)
    ↓ P/Invoke
GryceECLib.dll          ← 唯一对外 C API
    ↓ 链接 / 依赖
GryceCore.dll        ← ECS、Scene、Component、反射、Math
GryceRenderer.dll    ← RenderContext、RenderPipeline、Vulkan/OpenGL Backend
GrycePlatform.dll    ← Window 抽象、InputManager、Cursor、Timer
GrycePhysics.dll     ← Jolt 3D Physics、Box2D 2D Physics
GryceAudio.dll       ← miniaudio 音频（可选）
```

> 注：当前 `core/CMakeLists.txt` 把所有内容编译成一个 `gryce_core` 静态库。Phase 1 可以先让 `GryceECLib.dll` 静态链接 `gryce_core.lib`，再逐步拆分为独立 DLL，避免一次性改动过大。

---

## 2. 现有引擎入口梳理

### 2.1 ECS / 场景（`gryce_engine::ecs` / `gryce_engine::scene`）

| 文件 | 关键类型 | 说明 |
|------|---------|------|
| `core/ecs/world.h` | `ecs::World` | ECS 世界，持有 Scene 和 Systems |
| `core/scene/scene.h` | `scene::Scene` | 场景，单根节点树，管理 Entity 生命周期 |
| `core/scene/entity.h` | `scene::Entity` | 实体，UUID 标识，可挂 Component 和子实体 |
| `core/ecs/component_store.h` | `ecs::ComponentStore` | 组件存储池 |
| `core/ecs/system.h` | `ecs::ISystem` | System 基类，按 Phase 排序执行 |
| `core/ecs/systems/physics_system_3d.h` | `ecs::PhysicsSystem3D` | 3D 物理系统 |
| `core/ecs/systems/render_system_3d.h` | `ecs::RenderSystem3D` | 3D 渲染系统 |
| `core/ecs/systems/render_system_2d.h` | `ecs::RenderSystem2D` | 2D 渲染系统 |

**核心 API（编辑器当前用法）**：

```cpp
ecs::World world;
world.attach_scene(std::move(current_scene));
world.add_system<ecs::PhysicsSystem3D>();
world.add_system<ecs::RenderSystem3D>(&pipeline);
if (renderer2d) world.add_system<ecs::RenderSystem2D>(renderer2d.get());
world.init();

world.update(dt);           // PreUpdate / Update / PostUpdate
world.render(render_ctx);   // PreRender / Render / PostRender
```

**场景操作**：

```cpp
scene::Scene* s = world.scene();
scene::Entity* e = s->create_entity("Cube");
s->destroy_entity(e);
scene::Entity* found = s->find_entity_by_uuid(uuid);
scene::Entity* found = s->find_entity_by_name("Camera");
```

### 2.2 平台 / 窗口 / 输入（`gryce_engine::platform`）

| 文件 | 关键类型 | 说明 |
|------|---------|------|
| `core/platform/window.h` | `platform::Window` | GLFW 窗口封装 |
| `core/platform/input.h` | `platform::InputManager` | 输入状态管理（512 键 + 8 鼠标按钮） |
| `core/platform/cursor.h` | `platform::Cursor` | 光标 |

**关键 API**：

```cpp
platform::Window window("Gryce Editor", 1920, 1080, WindowMode::Windowed);
GLFWwindow* native = window.native_handle();  // 当前取 GLFW 句柄
#ifdef _WIN32
HWND hwnd = glfwGetWin32Window(native);       // 编辑器里就是这样取 HWND 的
#endif

platform::InputManager input;
input.update(&window);
bool pressed = input.is_key_pressed(GLFW_KEY_W);
bool held    = input.is_key_held(GLFW_KEY_W);
```

**HWND 注入点**：

- `Window` 目前只支持 GLFW 自己创建窗口。
- 需要新增一个 `platform::HostWindow` 或扩展 `Window`，让它接收外部 `HWND`，并提供与 GLFW 窗口相同的接口（`get_size`、`poll_events`、`swap_buffers` 等）。
- 输入可以复用 `InputManager`，但需要把 WPF 转发来的鼠标/键盘事件写进 `InputManager` 的内部状态，而不是从 GLFW 读。

### 2.3 渲染（`gryce_engine::render`）

| 文件 | 关键类型 | 说明 |
|------|---------|------|
| `core/render/render_context.h` | `render::RenderContext` | 高层渲染上下文，组合 backend + command buffer + render thread |
| `core/render/render.h` | `render::IRenderBackend` | 后端抽象接口（Vulkan / OpenGL） |
| `core/render/render_pipeline.h` | `render::RenderPipeline` | 渲染管线，Viewport / Game View 各一个 |
| `core/render/imgui_backend.h` | `render::IImGuiBackend` | ImGui 后端接口 |
| `core/render/opengl/gl_backend.h` | `render::GLBackend` | OpenGL 后端 |
| `core/render/vulkan/vk_backend.h` | `render::VulkanBackend` | Vulkan 后端 |

**关键 API**：

```cpp
render::RenderContext render_ctx;
render_ctx.set_validation_enabled(true);
render_ctx.init(window.native_handle(), render::RenderAPI::Vulkan);

auto renderer2d = render_ctx.create_renderer2d();
if (renderer2d) renderer2d->init(&render_ctx);

auto imgui_backend = render_ctx.create_imgui_backend();
render::ImGuiRenderer imgui;
imgui.init(window.native_handle(), std::move(imgui_backend));

render::RenderPipeline pipeline;
pipeline.set_viewport_output_enabled(true);
pipeline.set_imgui_backend(imgui.backend());
pipeline.init(&render_ctx, "res:/shaders");

render_ctx.start();     // 启动渲染线程
render_ctx.present();   // 提交一帧
```

**注意**：

- `RenderContext::init` 接受 `void* native_window`，实际就是 `GLFWwindow*`。
- Vulkan backend 内部会通过 `glfwCreateWindowSurface` 创建 surface；OpenGL backend 通过 `glfwMakeContextCurrent` 创建 context。
- 要支持外部 `HWND`，Vulkan backend 需要改用 `VkWin32SurfaceCreateInfoKHR` + `hwnd`；OpenGL backend 需要 `wglCreateContext` / `wglMakeCurrent` + `hwnd`。
- `RenderContext` 内部有渲染线程，但 WPF 已经要求 Core 不创建渲染线程，由外部按 144fps 驱动。因此 GryceECLib 可能不调用 `render_ctx.start()`，而是直接在 WPF 渲染线程里同步调用 `backend->begin_frame()` / `end_frame()`，或让 `RenderThread` 处于暂停状态。

### 2.4 物理（`gryce_engine::physics`）

| 文件 | 关键类型 | 说明 |
|------|---------|------|
| `core/physics/physics_world_3d.h` | `physics::IPhysicsWorld3D` | 3D 物理世界接口 |
| `core/physics/jolt_physics_world_3d.h` | `physics::JoltPhysicsWorld3D` | Jolt 实现 |
| `core/physics/physics_factory.cpp` | `physics::create_physics_world_3d` | 工厂函数 |
| `core/ecs/systems/physics_system_3d.h` | `ecs::PhysicsSystem3D` | ECS 系统封装 |

**关键 API**：

```cpp
auto phys_world = physics::create_physics_world_3d("Jolt");
phys_world->init(math::Vector3f(0, -9.81f, 0));
phys_world->step(dt, substeps);

physics::BodyHandle body = phys_world->create_body(desc);
phys_world->set_transform(body, pos, rot);
phys_world->get_transform(body, pos, rot);
std::optional<physics::RaycastHit> hit = phys_world->raycast(origin, dir, max_dist);
```

**ECS 集成**：

- `PhysicsSystem3D` 在 `on_init` 时扫描 Scene 中所有 `RigidBody` / `StaticBody` + Collider，创建对应物理 body。
- `on_update` 中调用 `phys_world->step(dt)`，并把物理结果同步回 Entity 的 Transform。
- `rebuild_body_for_entity(entity)` 用于组件增删后的热重载。

---

## 3. GryceECLib.dll 职责边界

GryceECLib 只做三件事：

1. **生命周期管理**：`Core_Init` / `Core_Shutdown`，创建/销毁 `RenderContext`、`World`、物理世界、输入状态。
2. **命令队列消费**：WPF 通过 `Core_SubmitCommand` 发送命令，GryceECLib 在 `Core_BeginFrame` 中把命令翻译成对 ECS / 物理 / 渲染的调用。
3. **回调触发**：把 Core 内部事件（实体选中、场景修改、PlayMode 变化等）通过 C 回调通知 WPF。

GryceECLib **不**做：

- 不创建窗口（WPF 提供 `HWND`）。
- 不创建渲染线程（WPF 在独立线程里按 144fps 调用 `Core_BeginFrame / Core_Render / Core_EndFrame`）。
- 不画任何编辑器面板（只画 Viewport Toolbar、Gizmo、Debug Overlay）。

---

## 4. 需要新增 / 修改的代码

### 4.1 平台层：支持外部 HWND

**方案 A（推荐）：新增 `platform::HostWindow`**

```cpp
namespace gryce_engine::platform {

class HostWindow {
public:
    explicit HostWindow(HWND hwnd);
    bool is_valid() const;
    void get_size(int& w, int& h) const;
    void set_size(int w, int h);
    void* native_handle() const;  // 返回 HWND
    // 输入由外部写入，这里不再从 GLFW 轮询
};

} // namespace gryce_engine::platform
```

- `HostWindow` 不调用 `glfwCreateWindow`，只保存 `HWND`。
- `RenderContext::init(hwnd, RenderAPI::Vulkan)` 要能识别这个 `HWND` 是外部句柄。
- Vulkan backend 需要新增 `init_with_hwnd(HWND)` 分支。
- OpenGL backend 在 Windows 下需要 `wgl` 相关代码。

**方案 B：扩展 `platform::Window`**

增加一个静态工厂：

```cpp
static Window wrap_external_handle(void* native_handle);
```

但 `Window` 目前基于 GLFW，改起来比新增类更脏。建议方案 A。

### 4.2 渲染层：适配无渲染线程模式

当前 `RenderContext` 默认使用渲染线程。 GryceECLib 需要一种“同步模式”：

```cpp
render_ctx.init(hwnd, RenderAPI::Vulkan);
// 不调用 render_ctx.start();
// 每帧：
render_ctx.backend()->begin_frame();
world.render(render_ctx);   // 同步执行渲染命令
imgui.render_draw_data(ImGui::GetDrawData());
render_ctx.backend()->end_frame();
```

但 `RenderContext` 的资源创建接口（`create_mesh` / `create_texture` 等）仍然可用，因为它们会直接调用 backend。

更稳妥的做法：保留渲染线程，但把 `RenderContext` 的调用方从“主线程”换成“WPF 渲染线程”。这样改动最小，只是线程角色变了。

### 4.3 ECS 层：提供 C 友好的实体标识

现有 `Entity` 用 `scene::UUID` 标识。C API 可以：

- 继续使用 UUID 字符串（C# 侧好处理）。
- 或者在 GryceECLib 内部维护一个 `int id → UUID` 的映射，对外暴露 `int`。

建议 Phase 1 先用 `int entity_id`，内部查表映射到 UUID，减少 WPF 侧改动。

### 4.4 命令映射

| C 命令 | 内部操作 |
|--------|---------|
| `ECmd_SelectEntity` | `scene->find_entity_by_id(id)` → `selected_entity = id` → callback |
| `ECmd_SetTransform` | `entity->transform()->set_position/rotation/scale` 或设置本地矩阵 → 标记 dirty |
| `ECmd_SetMaterial` | `entity->get_component<MeshRenderer>()->set_material(...)` |
| `ECmd_LoadScene` | `SceneSerializer::load_from_file(path)` → `world.attach_scene(...)` |
| `ECmd_PlayMode` | 保存场景快照 → `world.set_updates_enabled(true)` → callback |
| `ECmd_StopMode` | 恢复快照 → callback |
| `ECmd_ImportAsset` | 调用 `assets::AssetManager` / `import` 流程 |
| `ECmd_BuildProject` | 预留 |

### 4.5 ImGui Overlay

当前编辑器用 `render::ImGuiRenderer` + `IImGuiBackend`。 GryceECLib 需要：

- 在 Core 内部创建独立的 `ImGuiContext`。
- 用 `ImGui_ImplWin32_Init(hwnd)` + `ImGui_ImplVulkan_Init(...)` 或 `ImGui_ImplOpenGL3_Init`。
- 只绘制 Viewport Toolbar、ImGuizmo、Debug Info。
- 不加载编辑器主题/字体，避免和 WPF 主题冲突。

---

## 5. 推荐实施步骤

### Phase 1：最小可运行骨架（GryceECLib 链接 gryce_core）

1. 修改 `GameEngine/CMakeLists.txt`，让 `GryceECLib` 链接 `gryce_core`（静态库）。
2. 在 GryceECLib 内部创建 `HostWindow` 占位实现，接收 `HWND`。
3. 用 `RenderContext` + `World` + `PhysicsSystem3D` + `RenderSystem3D` 替换当前 `VulkanRHI` / `OpenGLRHI` / `SceneManager`。
4. 保留 Lock-free Command Queue 和 Callback Manager。
5. 实现 `Core_Render`：清屏 + ImGui Overlay + 调用 `world.render(render_ctx)` + present。
6. 编译通过，WPF 侧能初始化、收到回调、看到清屏色和 ImGui。

### Phase 2：场景操作命令

1. 实现 `ECmd_LoadScene`、 `ECmd_SelectEntity`、 `ECmd_SetTransform`。
2. 用 UUID → int 映射暴露实体 ID。
3. 集成 `Transform` 组件读写。
4. WPF 侧验证：加载场景、选中实体、Inspector 改 Transform。

### Phase 3：Play Mode 与物理

1. 实现 `ECmd_PlayMode` / `ECmd_StopMode`，场景快照/恢复。
2. 确认 `PhysicsSystem3D` 在 GryceECLib 中正确 step。
3. Gizmo 操作通过命令队列写回 Transform。

### Phase 4：拆分为独立 DLL（可选）

1. 把 `core/` 里的 platform / render / physics / ecs 拆成独立 CMake target。
2. 每个 target 导出符号（`export.h` 已经做了基础工作）。
3. GryceECLib 从链接 `gryce_core.lib` 改为链接多个 DLL。

---

## 6. 风险与注意事项

| 风险 | 说明 | 缓解方案 |
|------|------|---------|
| 渲染线程冲突 | `RenderContext` 内部有渲染线程，WPF 也要求在独立线程调用 Core | 使用同步模式，或不启动 `RenderContext` 的渲染线程 |
| ImGui 上下文冲突 | 编辑器已有 ImGui context，Core 内部再建一个 | 完全隔离：Core 只处理 Viewport 内 ImGui |
| RTTI / 异常 | 用户要求 C++20、noexcept、-fno-rtti | 检查 `gryce_core` 是否用 `dynamic_cast`（`World::get_system<T>` 用了） |
| 资源路径解析 | 现有引擎用 `res:/` 虚拟路径 | GryceECLib 初始化时需要设置 `project_root` |
| 静态库符号重复 | GryceECLib 和 Editor 都链接 imgui/glfw | 拆 DLL 后自然解决；Phase 1 需确保不重复链接 |
| C++23 vs C++20 | 现有引擎用 C++23，用户要求 C++20 | GryceECLib 自身用 C++20，但链接 `gryce_core.lib`（C++23 编译）通常兼容 |

---

## 7. 预计工作量

> 以下工作量为粗略估计，假设你熟悉现有代码且每天能投入一定时间。实际受编译/链接问题、第三方依赖调整影响较大。

| 阶段 | 主要工作 | 复杂度 | 估算 |
|------|---------|--------|------|
| Phase 1 | GryceECLib 链接 gryce_core，HWND 注入，RenderContext 同步模式，清屏 + ImGui Overlay | 高 | 较大 |
| Phase 2 | 场景加载、实体选择、Transform 设置命令 | 中 | 中等 |
| Phase 3 | Play Mode 快照/恢复、物理 step、Gizmo 回写 | 中高 | 中等偏大 |
| Phase 4 | 拆分 platform/render/physics/ecs 为独立 DLL | 高 | 较大 |

**关键难点排序**：

1. `RenderContext` 在不启动渲染线程的情况下稳定跑通 Vulkan（最难）。
2. 外部 `HWND` 注入到现有平台/渲染层（较难）。
3. 实体 ID 映射和命令队列到 ECS 的转换（中等）。
4. Play Mode 场景快照/恢复（中等）。

---

## 8. 下一步行动

需要你确认后再继续：

1. **是否接受 Phase 1 先让 GryceECLib 静态链接 `gryce_core.lib`？** 这是改动最小、最快的路径。
2. **渲染线程策略**：是不启动 `RenderContext` 的渲染线程（纯同步），还是让 WPF 渲染线程扮演原“主线程”角色？
3. **实体 ID 方案**：对外用 `int`（内部映射 UUID）还是直接用 UUID 字符串？
4. **是否立即开始 Phase 1 实现？** 还是需要我先给出更详细的某一块设计（比如 `HostWindow` 或 同步渲染模式）？
