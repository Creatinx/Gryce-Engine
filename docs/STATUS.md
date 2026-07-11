# Gryce Engine — Engineering Status

> 本文档客观描述各模块的当前实现状态，按模块组织，不包含优先级判断或执行建议。

---

## 1. 构建与部署

| 功能 | 状态 |
|---|---|
| CMake 多配置（Debug/Release） | 已实现 |
| MinGW 运行时 DLL 自动复制 | 已实现 |
| 统一输出目录（`bin/Debug`、`bin/Release`） | 已实现 |
| CI/本地一键构建脚本 | 已实现（`build.py`：一键 cmake + ninja + 编译器检测） |
| 发布包打包脚本 | 未实现 |
| 安装程序 | 未实现 |

---

## 2. 渲染（RHI）

### 2.1 后端抽象

| 功能 | 状态 |
|---|---|
| `IRenderBackend` 接口 | 已实现 |
| RHI 句柄化（`RHIMeshHandle` 等） | 已实现 |
| OpenGL 4.6 后端 | 已实现 |
| Vulkan 1.2 后端 | 已实现 |
| 命令缓冲合并批处理 | 已实现（`CommandStateCache` + multi-viewport 数组） |
| D3D12 / Metal 后端 | 未实现 |

### 2.2 OpenGL 后端

| 功能 | 状态 |
|---|---|
| VAO/VBO/IBO 封装 | 已实现 |
| 纹理上传与绑定 | 已实现 |
| 着色器编译与 uniform | 已实现 |
| FBO（shadow map、HDR、后处理） | 已实现 |
| 2D 批处理渲染 | 已实现 |
| 3D 网格渲染 | 已实现 |
| 帧率限制 | 已实现（CPU sleep） |
| NVIDIA `WGL_NV_delay_before_swap` | 已实现（`GLFramePacing` 检测 + 调用） |
| DSA（Direct State Access）路径 | 已实现（`gl_dsa_available()` 分支） |

### 2.3 Vulkan 后端

| 功能 | 状态 |
|---|---|
| 实例创建、验证层、调试 messenger | 已实现 |
| 物理设备选择、逻辑设备、队列 | 已实现 |
| 交换链、depth attachment、帧缓冲 | 已实现 |
| GPU 资源（mesh/texture/shader/framebuffer） | 已实现 |
| 2D 批处理 | 已实现 |
| ImGui Vulkan 后端 | 已实现 |
| VMA 集成 | 已实现（`vmaCreateBuffer` / `vmaCreateImage`） |
| Multi-viewport / secondary command buffer | 已实现（secondary CB pool + multi-viewport 数组） |

### 2.4 渲染管线（3D）

| 功能 | 状态 |
|---|---|
| PBR 着色（albedo/normal/roughness/metallic/ao） | 已实现 |
| Shadow map（2048x2048） | 已实现 |
| HDR + Tonemapping | 已实现 |
| 级联阴影（Cascaded Shadow Maps） | 未实现 |
| 天空盒（Cubemap） | 未实现 |
| IBL（Image-Based Lighting） | 未实现 |
| SSAO / SSR | 未实现 |
| GPU particles | 未实现 |
| 后处理栈（bloom、FXAA/TAA、motion blur、color grading） | 未实现 |

### 2.5 2D 渲染

| 功能 | 状态 |
|---|---|
| ColorRect、Label、Sprite2D、Circle、Polygon | 已实现 |
| TileMap | 已实现 |
| ParticleEmitter2D | 已实现 |
| ParallaxBackground | 已实现 |
| 2D Point Light、Directional Light、Spot Light | 未实现 |
| 2D 法线贴图 | 未实现 |
| 2D 阴影/遮挡 | 未实现 |
| 2D 后处理辉光（Bloom） | 未实现 |

### 2.6 材质系统

| 功能 | 状态 |
|---|---|
| Material 结构体（albedo/roughness/metallic/ao） | 已实现 |
| OpenGL UBO / Vulkan uniform buffer 上传 | 已实现 |
| 材质资源文件 `.gmat` | 未实现 |
| 材质编辑器 | 未实现 |
| Shader Graph | 未实现 |

---

## 3. ECS 与场景系统

### 3.1 ECS 核心

| 功能 | 状态 |
|---|---|
| Entity-Component-System 基础架构 | 已实现 |
| `World`、`Scene`、`Entity`、`Component` | 已实现 |
| 父子级 Transform 层级 | 已实现 |
| `ComponentFactory` 反射创建组件 | 已实现 |
| `on_awake`、`on_start` 回调 | 已实现 |
| `on_enable`、`on_disable` 回调 | 已实现 |
| System 优先级与依赖排序 | 已实现（同 phase 按 `priority()` 降序） |
| Entity 预制体（Prefab） | 已实现（`Prefab::load` + `Entity::clone`） |

### 3.2 场景序列化

| 功能 | 状态 |
|---|---|
| `.gesc` 场景文件 JSON 格式 | 已实现 |
| `res:/` 虚拟路径解析 | 已实现 |
| 保存/加载场景 | 已实现 |
| 场景差异保存 | 未实现 |
| 子场景/关卡流送 | 未实现 |
| 场景热重载 | 未实现 |

### 3.3 内置组件清单

| 组件 | 状态 |
|---|---|
| Transform | 已实现 |
| MeshRenderer | 已实现 |
| Camera | 已实现 |
| Light（Directional/Point/Spot） | 已实现 |
| RigidBody、StaticBody、BoxCollider（3D） | 已实现 |
| RigidBody2D、StaticBody2D、BoxCollider2D | 已实现 |
| PhysicalMaterial | 已实现 |
| DestructibleBody、FragmentBody | 已实现 |
| 2D 渲染组件（ColorRect/Label/Sprite2D/Circle/Polygon/TileMap/ParticleEmitter2D/ParallaxBackground） | 已实现 |
| AudioSource、AudioListener | 已实现（miniaudio 后端） |
| Animator、AnimationClip | 未实现 |
| NavMeshAgent、BehaviorTree | 未实现 |

### 3.4 预制体（Prefab）

| 功能 | 状态 |
|---|---|
| `.gesc` 作为 Prefab 加载 | 已实现（`Prefab::load`） |
| 实例化到任意 Scene | 已实现（`Prefab::instantiate`） |
| 深拷贝（新 UUID / EntityID、序列化级组件复制） | 已实现（`Entity::clone`） |
| 运行时变体（Prefab Variant） | 未实现 |
| 嵌套 Prefab | 未实现 |

---

## 4. 资源管线

### 4.1 资源管理器

| 功能 | 状态 |
|---|---|
| `AssetManager` 缓存 mesh/texture/material | 已实现 |
| `res:/` 路径虚拟化 | 已实现 |
| 资源引用计数 | 未实现 |
| 异步加载 | 部分实现（`AsyncLoader` 框架存在） |
| LRU 卸载 | 未实现 |
| 资源导入设置 `.gimport` | 未实现 |
| 资源包 `.gpack` | 未实现 |

### 4.2 模型加载

| 功能 | 状态 |
|---|---|
| OBJ 加载器 | 已实现 |
| FBX 加载器 | 未实现 |
| glTF 2.0 加载器 | 未实现 |
| Assimp 集成 | 接口已预留（`GRYCE_HAS_ASSIMP`） |
| 骨骼动画 | 未实现 |

### 4.3 纹理与材质

| 功能 | 状态 |
|---|---|
| PNG/JPG/BMP 加载（stb_image） | 已实现 |
| 法线/roughness/metallic/ao 贴图 | 已实现 |
| 纹理压缩（BC1~BC7/ASTC/ETC2） | 未实现 |
| Mipmap 自动生成 | 未实现 |
| HDR/EXR 环境贴图 | 未实现 |
| 材质预设库 | 未实现 |

### 4.4 字体与文本

| 功能 | 状态 |
|---|---|
| TTF 字体加载 + 动态图集（stb_truetype） | 已实现 |
| SDF（Signed Distance Field）字体 | 未实现 |
| 富文本标签 | 未实现 |
| 多语言/国际化 | 未实现 |

---

## 5. 物理系统

### 5.1 3D 物理

| 功能 | 状态 |
|---|---|
| AABB 碰撞检测与响应 | 已实现 |
| 重力、阻尼、弹力、摩擦力 | 已实现 |
| 刚体睡眠机制 | 已实现 |
| 碎裂系统（DestructibleBody/FragmentBody） | 已实现 |
| 稳定的刚体旋转/角速度 | 未实现 |
| 连续碰撞检测（CCD） | 未实现 |
| 射线检测（Physics.Raycast） | 未实现 |
| 角色控制器 | 未实现 |
| 关节系统（Hinge/Fixed/Spring/Distance） | 未实现 |
| Jolt Physics 集成 | 已实现（`GRYCE_HAS_JOLT`），默认优先，不可用时 fallback builtin 并告警 |

### 5.2 2D 物理

| 功能 | 状态 |
|---|---|
| 基础 AABB 碰撞 | 已实现 |
| 2D 刚体/静态体/碰撞盒 | 已实现 |
| 2D 圆形/多边形碰撞体 | 未实现 |
| 2D 关节与平台碰撞 | 未实现 |
| 2D 角色控制器 | 未实现 |
| Box2D 集成 | 已实现（`GRYCE_HAS_BOX2D=ON`），默认优先 2D 后端 |

### 5.3 碎裂与破坏

| 功能 | 状态 |
|---|---|
| 基于网格的立方体碎裂 | 已实现 |
| 碎裂参数可视化 | 未实现 |
| 碎裂后材质继承与 UV 保持 | 未实现 |
| 碎裂性能优化（对象池、异步生成） | 未实现 |

---

## 6. 音频系统

| 功能 | 状态 |
|---|---|
| miniaudio 集成 | 已实现（`audio/audio_engine.cpp`） |
| 音效/音乐加载与播放 | 部分实现 |
| 3D 空间音频 | 未实现 |
| 音频混音与总线 | 未实现 |
| WAV/OGG/MP3 支持 | 未完整测试 |

---

## 7. UI 系统

### 7.1 ImGui 集成

| 功能 | 状态 |
|---|---|
| OpenGL + Vulkan ImGui 后端 | 已实现 |
| Docking 支持 | 已实现 |
| DebugPanel（FPS、帧率限制、摄像机、输入） | 已实现 |
| ImGui 样式统一与主题文件 | 未实现 |
| ImGui 字体资源统一走引擎字体系统 | 未实现 |

### 7.2 引擎内置 UI（运行时）

| 功能 | 状态 |
|---|---|
| UI Canvas、Panel、Button、Image、Slider、Dropdown | 未实现 |
| UI 事件系统 | 未实现 |
| UI 动画与缓动 | 未实现 |
| UI 布局系统（Anchor、LayoutGroup） | 未实现 |

---

## 8. 脚本系统

| 功能 | 状态 |
|---|---|
| 脚本语言 | 未选型 |
| C++ 组件暴露到脚本 | 未实现 |
| 脚本生命周期回调 | 未实现 |
| 脚本热重载 | 未实现 |
| Visual Scripting | 未实现 |

---

## 9. 编辑器

| 功能 | 状态 |
|---|---|
| 场景视图（Scene View） | 未实现 |
| 游戏视图（Game View） | 未实现 |
| 层级面板（Hierarchy） | 未实现 |
| Inspector 面板 | 未实现 |
| 项目面板（Project） | 未实现 |
| 控制台面板（Console） | 未实现 |
| 动画/材质/地形编辑器 | 未实现 |
| 编辑器设置保存 | 未实现 |

---

## 10. 演示程序

### 10.1 3D 演示（3dtest）

| 功能 | 状态 |
|---|---|
| Cube + 地面 + PBR 材质 | 已实现 |
| FPS 相机（WASD、鼠标、Space/Ctrl） | 已实现 |
| 重力枪（鼠标左键拖拽） | 已实现 |
| 场景重置（R 键） | 已实现 |
| 材质预设与 ImGui 材质面板 | 已实现 |
| 更多可交互物体 | 未实现 |
| 碎裂后自动重置 | 未实现 |
| 导出为独立关卡 `.gesc` | 未实现 |

### 10.2 2D 演示（gt2dDemo）

| 功能 | 状态 |
|---|---|
| 平台跑酷基础 | 已实现 |
| 完整关卡（平台、坑、敌人、金币、终点） | 未实现 |
| 玩家角色（移动、跳跃、下蹲、受击、死亡、重生） | 未实现 |
| 敌人 AI（巡逻、追击、踩踏死亡） | 未实现 |
| 瓦片地图编辑器支持 | 未实现 |
| 2D 光照 | 未实现 |
| 音效 | 未实现 |
| UI（主菜单、HUD、暂停菜单、游戏结束） | 未实现 |
| 相机跟随与平滑插值 | 未实现 |

---

## 11. 性能与优化

| 功能 | 状态 |
|---|---|
| Vulkan/OpenGL 批处理 | 已实现 |
| GPU profiling（Nsight/RenderDoc 标记） | 未实现 |
| CPU profiling（Tracy/自带 profiler） | 未实现 |
| 遮挡剔除、视锥剔除 | 未实现 |
| GPU driven rendering | 未实现 |
| 纹理流送 | 未实现 |
| LOD 系统 | 未实现 |

---

## 12. 文档

| 文档 | 状态 |
|---|---|
| 架构文档（ARCHITECTURE.md） | 已完成 |
| 资源路径规范（PROJECT_LAYOUT.md） | 已完成 |
| 工程状态报告（STATUS.md） | 已完成 |
| Core API 规范（CORE_API.md） | 已完成 |
| API 文档（doxygen） | 未生成 |
| 用户手册 / 快速入门 | 未编写 |
| 着色器编写指南 | 未编写 |
