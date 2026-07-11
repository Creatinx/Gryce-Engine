# Gryce Engine 完整开发 Todo List

> 本文件由 AI 生成初稿，开发者可在此基础上增删改；改完后由 AI 按清单逐项落实。
> 状态：`[x]` 已完成，`[~]` 进行中，`[ ]` 待做，`[?]` 待决策。

---

## P0 引擎底层（Blocker，必须先稳定）

### P0.1 构建与部署
- [x] 统一 Debug/Release 输出目录（`build/debug/bin/Debug`、`build/release/bin/Release`）。
- [x] MinGW 运行时 DLL 自动复制（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`、`glew32.dll`、`glfw3.dll`）。
- [x] 清理 stale 可执行文件（旧 `gryce.exe`、`2dtest.exe`、`gryce_vk_sdk.exe`）。
- [ ] 增加 CI/本地构建脚本：`build_debug.bat` / `build_release.bat`（一键 cmake + ninja + 复制 res）。
- [ ] 发布包打包脚本：自动收集 exe、dll、res、shaders、scenes、fonts。
- [ ] 安装程序 / 绿色包二选一（Windows 默认绿色包）。

### P0.2 RHI（Render Hardware Interface）
- [x] 抽象接口：`IRenderBackend`、`IShader`、`ITexture`、`IFrameBuffer`、`IRenderer2D`、`IRenderer3D`。
- [x] OpenGL 4.6 后端完整实现。
- [x] Vulkan 1.2 后端完整实现。
- [ ] RHI 资源统一句柄化：用 `RHITextureHandle`、`RHIMeshHandle` 替代裸指针，避免跨线程释放。
- [ ] RHI 命令缓冲合并批处理：把 `push_command` 的小 lambda 按类型合并（draw/dispatch/barrier）。
- [ ] Vulkan：集成 VMA（Vulkan Memory Allocator）替代裸 `vkAllocateMemory`。
- [ ] Vulkan：Multi-viewport / secondary command buffer 支持。
- [ ] OpenGL：DSA（Direct State Access）路径，减少状态切换。
- [ ] D3D12 / Metal 后端调研（后续可选）。

### P0.3 渲染管线
- [x] 3D PBR 着色（albedo / normal / roughness / metallic / ao）。
- [x] Shadow map（2048x2048 级联阴影）。
- [x] HDR + Tonemapping。
- [x] 2D 批处理渲染（ColorRect、Label、Sprite、Circle、Polygon、Line）。
- [ ] 2D 光照系统：
  - [ ] 2D Point Light、Directional Light、Spot Light 组件。
  - [ ] 2D 法线贴图支持（SpriteNormalMap）。
  - [ ] 2D 阴影/遮挡（SDF shadow 或硬阴影）。
  - [ ] 2D 后处理辉光（Bloom）用于子弹/恒星发光。
- [ ] 3D 渲染完善：
  - [ ] 天空盒（Cubemap）。
  - [ ] IBL（Image-Based Lighting）。
  - [ ] SSAO / SSR（可选）。
  - [ ] 级联阴影（Cascaded Shadow Maps）。
  - [ ] 粒子系统（GPU particles）。
  - [ ] 后处理栈：bloom、FXAA/TAA、motion blur、color grading。
- [ ] 材质系统：
  - [ ] 材质资源文件 `.gmat`（JSON）。
  - [ ] 材质编辑器（Inspector 中可视化编辑）。
  - [ ] Shader Graph / 节点材质（远期）。

### P0.4 线程与同步
- [x] 独立渲染线程 + 命令队列。
- [x] 帧延迟资源销毁（pending destroys）。
- [ ] 帧率控制：
  - [x] 基础 CPU sleep 限帧。
  - [ ] NVIDIA `WGL_NV_delay_before_swap` 接入（当前不可用）。
  - [ ] Vulkan `VK_GOOGLE_display_timing` / `VK_EXT_swapchain_maintenance1`。
- [ ] 画面不变时不重复提交：比较上一帧与当前帧 command buffer 哈希，相同则跳过 GPU 提交，但逻辑继续更新。
- [ ] Job System：线程池 + task graph，替代单渲染线程。

---

## P1 ECS 与场景系统

### P1.1 ECS 核心
- [x] Entity-Component-System 基础架构。
- [x] `World`、`Scene`、`Entity`、`Component` 类 Godot/Unity 混合风格。
- [x] 父子级 Transform 层级。
- [x] `component_factory` 反射创建组件。
- [ ] Component 生命周期：
  - [ ] `on_awake`、`on_start`、`on_enable`、`on_disable`、`on_destroy` 回调。
  - [ ] 场景加载后自动调用 `on_start`。
- [ ] System 优先级与依赖排序。
- [ ] Entity 预制体（Prefab）：`.prefab` 文件 + 运行时实例化。

### P1.2 场景序列化
- [x] `.gesc` 场景文件 JSON 格式。
- [x] `res:/` 虚拟路径解析为项目根目录。
- [x] 保存/加载场景。
- [ ] 场景差异保存（只保存修改过的实体）。
- [ ] 子场景/关卡流送（Level Streaming）。
- [ ] 场景热重载：文件变化后自动重载，保留运行状态。

### P1.3 内置组件清单
- [x] `Transform`（2D/3D 统一）。
- [x] `MeshRenderer`（3D）。
- [x] `Camera`（2D/3D）。
- [x] `Light`（Directional/Point/Spot）。
- [x] `RigidBody`、`StaticBody`、`BoxCollider`（3D）。
- [x] `RigidBody2D`、`StaticBody2D`、`BoxCollider2D`（2D）。
- [x] `PhysicalMaterial`。
- [x] `DestructibleBody`、`FragmentBody`（3D 碎裂）。
- [x] 2D 渲染组件：`ColorRect`、`Label`、`Sprite2D`、`Circle`、`Polygon`、`TileMap`、`ParticleEmitter2D`、`ParallaxBackground`。
- [ ] 音频组件：`AudioSource`、`AudioListener`。
- [ ] 动画组件：`Animator`、`AnimationClip`。
- [ ] 导航/AI 组件：`NavMeshAgent`、`BehaviorTree`（远期）。

---

## P2 资源管线

### P2.1 资源管理器
- [x] `AssetManager` 缓存 mesh、texture、material。
- [x] `res:/` 路径虚拟化。
- [ ] 资源引用计数 + 异步加载 + LRU 卸载。
- [ ] 资源导入设置 `.gimport`（纹理压缩、mipmap、法线类型）。
- [ ] 资源包（`.gpack`）用于发布时打包资源。

### P2.2 模型加载
- [x] OBJ 加载器。
- [ ] FBX 加载器（`assimp` 或自研）。
- [ ] glTF 2.0 加载器。
- [ ] PLY / STL / DAE 加载器。
- [ ] 模型导入设置：坐标系转换、缩放、法线重新计算、切线生成。
- [ ] 骨骼动画（skinned mesh）支持。

### P2.3 纹理与材质
- [x] PNG/JPG/BMP 纹理加载（stb_image）。
- [x] 法线/roughness/metallic/ao 贴图。
- [ ] 纹理压缩：BC1~BC7 / ASTC / ETC2。
- [ ] 纹理 Mipmap 自动生成。
- [ ] HDR / EXR 环境贴图加载。
- [ ] 材质预设库：Metal、Concrete、Wood、Plastic、Glass、Water 等。

### P2.4 字体与文本
- [x] TTF 字体加载 + 动态图集（stb_truetype）。
- [ ] SDF（Signed Distance Field）字体，支持放大不虚。
- [ ] 富文本标签（颜色、大小、粗体）。
- [ ] 多语言/国际化（i18n）与字体回退。

### P2.5 音频
- [ ] 集成 miniaudio：音效/音乐加载与播放。
- [ ] 3D 空间音频（位置、衰减、多普勒）。
- [ ] 音频混音与总线（Master/SFX/Music/Voice）。
- [ ] 支持 WAV、OGG、MP3。

---

## P3 物理系统

### P3.1 3D 物理
- [x] AABB 碰撞检测与响应。
- [x] 重力、阻尼、弹力、摩擦力。
- [x] 刚体睡眠机制。
- [x] 碎裂系统（DestructibleBody、FragmentBody）。
- [ ] 稳定的刚体旋转/角速度。
- [ ] 连续碰撞检测（CCD，防止高速穿透）。
- [ ] 射线检测（Physics.Raycast）。
- [ ] 角色控制器（CharacterController）。
- [ ] 关节系统：Hinge、Fixed、Spring、Distance。
- [ ] 物理材质可视化编辑器。
- [ ] 集成 Bullet / Jolt Physics（远期替代自研）。

### P3.2 2D 物理
- [x] 基础 AABB 碰撞。
- [x] 2D 刚体/静态体/碰撞盒。
- [ ] 2D 圆形/多边形碰撞体。
- [ ] 2D 关节与平台碰撞（one-way platform）。
- [ ] 2D 角色控制器。
- [ ] 集成 Box2D（可选）。

### P3.3 碎裂与破坏
- [x] 基于 Voronoi/网格的立方体碎裂。
- [ ] 碎裂参数可视化（threshold、impulse、segments、lifetime）。
- [ ] 碎裂后材质继承与 UV 保持。
- [ ] 碎裂性能优化（对象池、异步生成）。

---

## P4 UI 系统

### P4.1 ImGui 集成
- [x] OpenGL + Vulkan ImGui 后端。
- [x] Docking 支持。
- [x] DebugPanel：FPS、帧率限制、摄像机、输入切换。
- [ ] ImGui 样式统一与主题文件。
- [ ] ImGui 字体资源统一走引擎字体系统。

### P4.2 引擎内置 UI（运行时）
- [ ] UI Canvas、Panel、Button、Image、Slider、Dropdown。
- [ ] UI 事件系统（on_click、on_hover、on_value_changed）。
- [ ] UI 动画与缓动。
- [ ] UI 布局系统（Anchor、LayoutGroup）。

---

## P5 脚本系统

- [ ] 脚本语言选型（Lua / C# / 自研可视化脚本）。
- [ ] C++ 组件暴露到脚本。
- [ ] 脚本生命周期：`start`、`update`、`on_collision_enter` 等。
- [ ] 脚本热重载。
- [ ] Visual Scripting 节点编辑器（远期）。

---

## P6 编辑器（Editor）

- [ ] 场景视图（Scene View）：Gizmo、网格、相机预览。
- [ ] 游戏视图（Game View）。
- [ ] 层级面板（Hierarchy）。
- [ ] Inspector 面板：组件属性编辑。
- [ ] 项目面板（Project）：资源浏览、导入、拖放。
- [ ] 控制台面板（Console）：日志过滤、跳转错误。
- [ ] 动画编辑器、材质编辑器、地形编辑器（远期）。
- [ ] 编辑器设置保存（`editor.settings`）。

---

## P7 演示程序

### P7.1 3D 演示（3dtest）
- [x] Cube + 地面 + PBR 材质。
- [x] FPS 相机（WASD、鼠标、Space/Ctrl）。
- [x] 重力枪（鼠标左键拖拽）。
- [x] `R` 重置场景。
- [x] 材质预设与 ImGui 材质面板。
- [ ] 添加更多可交互物体（球体、胶囊体、不同材质）。
- [ ] 按 `F2` 碎裂后自动重置按钮。
- [ ] 3D 演示导出为独立关卡 `.gesc`。

### P7.2 2D 演示（gt2dDemo）
- [x] 马里奥式平台跑酷基础。
- [ ] 完整关卡：平台、坑、敌人、金币、终点旗。
- [ ] 玩家角色：移动、跳跃、下蹲、受击、死亡、重生。
- [ ] 敌人 AI：巡逻、追击、踩踏死亡。
- [ ] 瓦片地图（TileMap）关卡编辑器支持。
- [ ] 2D 光照：玩家手电筒、发光子弹、恒星、金币辉光。
- [ ] 音效：跳跃、收集、受伤、BGM。
- [ ] UI：主菜单、HUD、暂停菜单、游戏结束。
- [ ] 相机跟随与平滑插值。

---

## P8 性能与优化

- [x] Vulkan/OpenGL 批处理。
- [ ] GPU profiling：Nsight / RenderDoc 标记。
- [ ] CPU profiling：Tracy /自带 profiler。
- [ ] Occlusion culling、frustum culling。
- [ ] GPU driven rendering（indirect draw、mesh shader 远期）。
- [ ] 纹理流送（Texture Streaming）。
- [ ] LOD 系统。

---

## P9 光追与高级渲染（硬目标，远期）

- [ ] Vulkan Ray Tracing（`VK_KHR_ray_tracing_pipeline` / `VK_KHR_ray_query`）。
- [ ] 实时光追反射/阴影/全局光照。
- [ ] 路径追踪离线渲染模式。
- [ ] DLSS/FSR/XeSS 超分辨率（远期）。

---

## P10 发布准备

- [ ] 版本号自动化（CMake 注入 + Git 标签）。
- [ ] 崩溃报告与 minidump（Windows）。
- [ ] 日志系统改进：分级、文件滚动、崩溃时转储。
- [ ] 配置文件系统（`config.ini` / JSON）。
- [ ] Steam/Epic 上架准备（远期）。
- [ ] 文档：
  - [ ] 架构文档。
  - [ ] API 文档（doxygen）。
  - [ ] 用户手册 / 快速入门。
  - [ ] 着色器编写指南。

---

## 当前 blocker（需要人确认）

1. `[?]` 2D 演示方向：继续完善马里奥式跑酷，还是改成之前的飞机打陨石？
2. `[?]` 脚本语言选型：Lua / C# / 其他？
3. `[?]` 是否立刻替换自研物理为 Bullet/Box2D？
4. `[?]` 是否优先做编辑器，还是优先完善运行时与演示？

---

## 使用说明

1. 直接编辑本文件，勾选/增删/调整任务。
2. 改完后告知 AI，AI 会按从上到下的顺序逐项实现。
3. 每完成一项，AI 会更新本文件中的对应复选框。
