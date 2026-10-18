# 引擎架构

本文描述 Gryce Engine 的总体结构：二进制分层、各模块职责与它们之间的边界。渲染后端的内部实现见 [renderer.md](renderer.md)，构建方式见 [build.md](build.md)。

## 1. 二进制分层

引擎按三个动态库加一个宿主程序组织，命名空间统一为 `gryce_engine`：

| 目标 | 内容 |
| --- | --- |
| `GryceCore.dll` | 抽象层、场景 / ECS / 组件 / 反射 / 资源 / 渲染抽象、物理与音频 |
| `GryceRenderer.dll` | 渲染管线与各后端（OpenGL / Vulkan / D3D12） |
| `GrycePlatform.dll` | 窗口、输入、光标 |
| `GryceRenderSmoke.exe` | 演示宿主，人工运行，不参与 CTest |

对外能力经 `engine/api/` 的 C API 暴露，引擎以 DLL 形式被集成。API 按域拆分：`core` / `entity` / `scene` / `component` / `render` / `material` / `asset` / `animator` / `input` / `platform` / `window` / `viewport` / `script`。句柄经 `runtime/entity_handle_map.h` 管理，跨边界只传稳定句柄与 POD。

## 2. 模块职责

`engine/` 下的目录划分：

| 目录 | 职责 |
| --- | --- |
| `api/` | C API 边界，各域一个文件对 |
| `math/` | 向量、矩阵、四元数、相机、射线（右手坐标系） |
| `scene/` | 实体、场景、预制体、`.gesc` 序列化、UUID |
| `ecs/` | `World`、`ComponentStore` 与各 System |
| `components/` | 组件定义，按 2d / 3d / common 分组 |
| `reflection/` | 反射注册与内建类型反射 |
| `render/` | 渲染抽象层、管线实现与各后端 |
| `assets/` | 模型 / 图像加载、材质推导、LOD、顶点优化、异步加载 |
| `resources/` | 工程配置、`res:/` 路径、Pak / Gpack、AES-GCM |
| `animation/` | 骨骼、动画剪辑、姿态混合 |
| `platform/` | 窗口、输入、光标 |
| `runtime/` | 引擎上下文、实体句柄表、命令缓冲 |
| `utils/` | 日志（glog）、帧率限制、加解密原语 |

## 3. ECS 与反射

- `ecs/world.h` 持有实体与组件存储；`ecs/component_store.h` 按组件类型密集存储。
- 系统位于 `ecs/systems/`：`animator` / `audio` / `hierarchy` / `physics_2d` / `physics_3d` / `render_2d` / `render_3d` / `subviewport`。Play 模式驱动这些系统更新，退出后恢复编辑态快照。
- 组件经 `reflection/` 注册，因此可被序列化、编辑器与脚本统一访问；组件创建走 `components/component_factory.h`。

## 4. 场景与序列化

场景为 JSON 文本格式（`.gesc`，当前 version 2），实体由 `name` / `uuid` / `parent` / `enabled` / `transform` / `components` 组成。序列化器位于 `scene/scene_serializer.cpp`，预制体位于 `scene/prefab.cpp`。

多材质模型导入时，`scene/multi_material_assembly.cpp` 把子网格拆成父实体加子实体，子实体经 `copy_settings_from` 继承父实体材质参数（见 [model-import.md](model-import.md)）。

## 5. 资源系统

- 虚拟路径 `res:/` 由 `resources/resource_path.cpp` 解析，工程根由 `project.data` 指定。
- `assets/asset_manager.cpp` 是模型与图像的唯一入口；`assets/async_loader.cpp` 提供异步加载。
- 打包格式：`resources/pak_bundle.cpp`（`.pak` v3 / v4 加密）与 `resources/gpack_bundle.cpp`（`.gpack` v1）；加密走 `resources/aes_gcm.cpp`（AES-256-GCM，与 GryceGC 的 `encrypt` 字节兼容，见 [toolchain.md](toolchain.md)）。
- 运行期热重载：场景、着色器、材质与网格支持重载，无需重启进程。

## 6. 渲染抽象

渲染侧的核心约束是"逐帧状态写入必须走 `RenderContext` 命令缓冲"：

- `render/render_context.h` 收集命令，`render/render_thread.cpp` 在独立线程执行；`sync_mode` 下命令在主线程内联执行（WebGL 构建即用此模式，见 [webgl-backend.md](webgl-backend.md)）。
- 后端经 `render/render.h` 的 `IRenderBackend` 接口接入，能力缺失时按 `RenderBackendCapabilities` 声明式降级（例如无 compute shader 则回退 CPU 剔除）。
- GPU 资源位于 `render/storage_rd/`，管线实现位于 `render/renderer_rd/`（`forward_clustered` / `shadow` / `effects` / `environment`）。

## 7. 平台层

`platform/window.h` 封装 GLFW 窗口与上下文，`platform/input.h` 处理键盘鼠标。桌面端支持交互模式（鼠标锁定转视角、WASD 移动）；Web 端受浏览器策略约束有额外处理，见 [webgl-backend.md](webgl-backend.md)。
