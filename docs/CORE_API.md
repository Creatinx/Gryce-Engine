# Gryce Engine — Core API Specification

> 本文档面向使用 Gryce Engine `core` 模块的外部开发者，定义公共 API 接口、使用规范与集成方式。

---

## 1. 库类型

`gryce_core` 默认构建为**静态库**（`.lib` / `.a`）。

通过 CMake 选项可切换为动态库：

```cmake
set(GRYCE_BUILD_SHARED ON)  # 默认 OFF
```

静态库模式下，`GRYCE_API` 宏为空，所有符号直接内联到最终可执行文件中。

---

## 2. CMake 集成

### 2.0 构建环境

| 项 | 要求 |
|---|---|
| 平台 | Windows 10/11 |
| 编译器 | **MinGW-w64 GCC**（推荐 MSYS2 UCRT64） |
| CMake | ≥ 3.28 |
| 生成器 | Ninja（推荐） |

> **注意**：在默认 Windows PowerShell / CMD 中运行 CMake 时，若未安装 MSYS2 或未将 MinGW 加入 PATH，CMake 可能自动检测到 MSVC 并因缺少 `rc.exe` / `mt.exe` 而失败。请使用以下任一方式：
> 1. **推荐**：在 MSYS2 UCRT64 终端中构建。
> 2. 显式指定 MinGW 编译器：`-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`
> 3. 若使用 MSVC，必须在 **x64 Native Tools Command Prompt for VS 2022** 中运行。

根 CMakeLists.txt 已内置自动检测：如果未指定编译器，会尝试在 `C:/msys64/ucrt64/bin` 或 `C:/msys64/mingw64/bin` 中自动找到 MinGW GCC 并锁定。

### 2.1 作为子目录引入

```cmake
# 在你的 CMakeLists.txt 中
add_subdirectory(path/to/Gryce-Engine/core)

target_link_libraries(your_app PRIVATE gryce_core)
```

### 2.2 作为 install target 引入

```cmake
# 在 Gryce-Engine 中安装
cmake --install . --prefix /path/to/install

# 在你的项目中
find_package(GryceCore REQUIRED)
target_link_libraries(your_app PRIVATE Gryce::GryceCore)
```

### 2.3 头文件包含路径

`gryce_core` 的 `target_include_directories` 已自动暴露以下目录：

- `core/` 本身（`#include "scene/entity.h"`）
- `third_party/`（ImGui、nlohmann/json、stb）
- `third_party/stb/`（stb_image、stb_truetype）

外部项目无需额外指定 include 路径。

---

## 3. 命名空间

所有公共 API 位于 `gryce_engine` 命名空间及其子命名空间下：

| 命名空间 | 内容 |
|---|---|
| `gryce_engine::scene` | Scene、Entity、UUID、SceneSerializer |
| `gryce_engine::ecs` | World、ISystem、ComponentStore、EntityID |
| `gryce_engine::components` | Component 基类、Transform、所有具体组件 |
| `gryce_engine::render` | IRenderBackend、RenderContext、IRenderer2D、Material |
| `gryce_engine::math` | Vector2f/3f/4f、Matrix4f、Quaternionf、Camera |
| `gryce_engine::platform` | Window、Input、Cursor |
| `gryce_engine::assets` | AssetManager、MeshData、TextureData、AssetHandle |
| `gryce_engine::resources` | ResourcePath、Project、Tileset |
| `gryce_engine::physics` | IPhysicsWorld2D/3D、PhysicsFactory、PhysicsTypes |
| `gryce_engine::utils` | FrameLimiter、glog |

---

## 4. 公共头文件清单

以下头文件为 `gryce_core` 的公共接口，可通过 `#include "<path>"` 直接引用：

### 场景与 ECS

```
scene/entity.h
scene/scene.h
scene/scene_serializer.h
scene/uuid.h
ecs/world.h
ecs/system.h
ecs/types.h
ecs/query.h
ecs/component_store.h
```

### 组件

```
components/component.h
components/component_factory.h
components/transform.h
components/camera.h
components/light.h
components/mesh_renderer.h
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
components/2d/component_2d.h
components/2d/basic_rect.h
components/2d/shape.h
components/2d/label.h
components/2d/light_2d.h
components/2d/sprite_2d.h
components/2d/tilemap.h
components/2d/camera_2d.h
components/2d/parallax_background.h
components/2d/particle_emitter.h
```

### 渲染

```
render/render.h              # IRenderBackend、create_render_backend()
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
physics/builtin_physics_world_2d.h
physics/builtin_physics_world_3d.h
```

### 工具

```
utils/frame_limiter.h
utils/glog/glog_lib.h
export.h                     # GRYCE_API 宏
```

---

## 5. API 使用规范

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

### 5.2 World 与 System

```cpp
#include "ecs/world.h"
#include "ecs/systems/physics_system.h"
#include "ecs/systems/render_system_3d.h"

using namespace gryce_engine;

auto world = std::make_unique<ecs::World>();

// 加载场景
auto scene = scene::Scene::load("res:/scenes/main.gesc");
world->attach_scene(std::move(scene));

// 注册系统
world->add_system<ecs::PhysicsSystem>();
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

// 创建渲染后端
auto backend = render::create_render_backend(render::RenderAPI::OpenGL);
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

---

## 6. 扩展规范

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

## 7. 线程安全

| 接口 | 线程安全 |
|---|---|
| `World::update()` | 主线程 |
| `Entity::add_component()` | 主线程（初始化阶段） |
| `RenderContext::push_command()` | 主线程 |
| `IRenderBackend::*` | 渲染线程（通过命令队列间接调用） |
| `AssetManager::load_*()` | 主线程（内部有异步加载，返回前同步等待） |

---

## 8. 编译宏

| 宏 | 条件 | 说明 |
|---|---|---|
| `GRYCE_HAS_VULKAN` | Vulkan SDK 可用 | 暴露 Vulkan 后端头文件与实现 |
| `GRYCE_HAS_IMGUI` | ImGui 源可用 | 编译 ImGui 后端集成 |
| `GRYCE_HAS_BOX2D` | Box2D 可用 | 启用 Box2D 物理后端 |
| `GRYCE_HAS_JOLT` | Jolt Physics 可用 | 启用 Jolt 物理后端 |
| `GRYCE_HAS_ASSIMP` | Assimp 可用 | 启用 Assimp 模型导入 |
| `GRYCE_BUILD_SHARED` | 显式设置 ON | 构建动态库而非静态库 |

---

## 9. 依赖清单

构建 `gryce_core` 所需的外部依赖：

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
