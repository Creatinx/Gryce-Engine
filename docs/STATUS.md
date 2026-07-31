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
| 后端分层（`RenderAPI` 枚举 + 工厂） | 已实现（Vulkan 默认 / OpenGL 兼容 / DX11、DX12 为 WinNative 预留，`create_render_backend` 返回 `nullptr`） |
| OpenGL 4.6 后端 | 已实现（兼容后端） |
| Vulkan 1.2 后端 | 已实现（**默认后端**） |
| 命令缓冲合并批处理 | 已实现（`CommandStateCache` + multi-viewport 数组） |
| 渲染 API 项目设置 | 已实现（Project Settings 窗口渲染 API 下拉 + Render Quality 区，持久化到 `project_settings.json` 的 `graphics` 组，重启生效） |
| D3D11 / D3D12 / Metal 后端 | 未实现（DX 枚举值已预留） |

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
| PBR 着色（albedo/normal/roughness/metallic/ao/emissive） | 已实现 |
| 多光源（最多 8 盏：方向光/点光/聚光，逐光源 range/cone 参数） | 已实现 |
| 环境光（ambient） | 已实现 |
| Shadow map（尺寸可配置，取第一个方向光） | 已实现 |
| 阴影光空间贴合相机视锥（8 角点 → 光空间 AABB，纹素对齐，深度向光源延伸 50 单位，无固定 cutoff） | 已实现 |
| 阴影边缘淡出（4 个 PBR 着色器内 smoothstep 5%） | 已实现 |
| 自适应 shadow bias（基础值 + 0.5 纹素）+ Vulkan 硬件 slope-scaled depth bias（1.25 / 2.5） | 已实现 |
| 天空盒（Cubemap，GL+VK，ITexture::upload_cubemap） | 已实现 |
| 半透明渲染（按相机距离排序，blend + depth write off） | 已实现 |
| HDR + Tonemapping（None/Reinhard/ACES + exposure） | 已实现 |
| 级联阴影（Cascaded Shadow Maps） | 未实现 |
| IBL（Image-Based Lighting） | 已实现（OpenGL/Vulkan PBR + Skinned PBR， irradiance/prefilter/BRDF LUT） |
| SSAO / SSR | 未实现 |
| GPU particles | 未实现 |
| 后处理栈（bloom、FXAA/TAA、motion blur、color grading） | 部分实现（Bloom 已实现；FXAA/TAA、motion blur、color grading 待实现） |

### 2.5 2D 渲染

| 功能 | 状态 |
|---|---|
| ColorRect、Label、Sprite2D、Circle、Polygon | 已实现 |
| TileMap | 已实现 |
| ParticleEmitter2D | 已实现 |
| ParallaxBackground | 已实现 |
| 2D Point Light、Directional Light、Spot Light | 已实现 |
| 2D 法线贴图 | 已实现（`Sprite2D::normal_map_path` + `IRenderer2D::draw_lit_sprite`） |
| 2D 阴影/遮挡 | 已实现 |
| 2D 后处理辉光（Bloom） | 已实现（OpenGL / Vulkan） |

### 2.6 材质系统

| 功能 | 状态 |
|---|---|
| Material（albedo/roughness/metallic/ao + emissive/opacity/blend_mode/two_sided/UV 变换） | 已实现 |
| OpenGL UBO / Vulkan uniform buffer 上传 | 已实现 |
| 材质资源文件 `.gmat`（JSON，支持 res:/ 虚拟路径） | 已实现 |
| OBJ MTL / Assimp 材质导入（随 MeshRenderer 上传时按默认值合并） | 已实现 |
| 材质编辑器 | 已实现（`editor/ui/material_editor_window.*`：PBR 参数、贴图槽、物理属性；保存后即时 re-upload GPU 纹理） |
| Shader Graph | 未实现 |

---

## 3. ECS 与场景系统

### 3.1 ECS 核心

| 功能 | 状态 |
|---|---|
| Entity-Component-System 基础架构 | 已实现 |
| `World`、`Scene`、`Entity`、`Component` | 已实现 |
| 场景单根节点（合成根 Entity，`Scene::root()`） | 已实现（`.gesc` 格式版本 2，v1 兼容加载；Hierarchy 顶行显示场景名且不可删除） |
| 父子级 Transform 层级 | 已实现 |
| 2D 父链变换（`world_transform_2d()` 组合祖先 XY/Z 旋转/XY 缩放） | 已实现（`Node2D::top_level` 可脱离父链；`z_index` 参与 2D 排序；2D 物理与编辑器 2D Gizmo 使用世界 2D 空间） |
| `ComponentFactory` 反射创建组件 | 已实现 |
| `on_awake`、`on_start` 回调 | 已实现 |
| `on_enable`、`on_disable` 回调 | 已实现 |
| System 优先级与依赖排序 | 已实现（同 phase 按 `priority()` 降序） |
| Entity 预制体（Prefab） | 已实现（`Prefab::load` + `Entity::clone`） |

### 3.2 场景序列化

| 功能 | 状态 |
|---|---|
| `.gesc` 场景文件 JSON 格式 | 已实现（版本 2；v1 文件原样兼容加载，格式保持扁平） |
| `res:/` 虚拟路径解析 | 已实现 |
| 保存/加载场景 | 已实现 |
| 场景差异保存 | 已实现（`Scene::serialize_delta` / `save_delta`） |
| 子场景/关卡流送 | 已实现（`Scene::stream_in` / `stream_out`） |
| 场景热重载 | 已实现（`Scene::hot_reload`，保留运行时状态） |

### 3.3 内置组件清单

| 组件 | 状态 |
|---|---|
| Transform | 已实现 |
| MeshRenderer | 已实现 |
| Camera | 已实现 |
| Light（Directional/Point/Spot） | 已实现 |
| RigidBody、StaticBody、BoxCollider（3D） | 已实现 |
| RigidBody2D、StaticBody2D、BoxCollider2D、CircleCollider2D | 已实现 |
| CharacterController3D、CharacterController2D | 已实现 |
| Joint3D、Joint2D | 已实现 |
| PhysicalMaterial | 已实现 |
| DestructibleBody、FragmentBody | 已实现 |
| 2D 渲染组件（ColorRect/Label/Sprite2D/Circle/Polygon/TileMap/ParticleEmitter2D/ParallaxBackground） | 已实现 |
| AudioSource、AudioListener | 已实现（miniaudio 后端） |
| 骨骼动画（Skeleton / AnimationClip / SkinnedMeshRenderer / AnimatorSystem） | 已实现 |
| Animator Controller（状态机 / Blend Tree） | 未实现 |
| NavMeshAgent、BehaviorTree | 未实现 |

### 3.4 预制体（Prefab）

| 功能 | 状态 |
|---|---|
| `.gesc` 作为 Prefab 加载 | 已实现（`Prefab::load`） |
| 实例化到任意 Scene | 已实现（`Prefab::instantiate`） |
| 深拷贝（新 UUID / EntityID、序列化级组件复制） | 已实现（`Entity::clone`） |
| 实例覆盖参数（`overrides`：transform、components、remove） | 已实现 |
| 嵌套 Prefab | 已实现（Prefab 文件内可引用其他 Prefab） |
| 还原模板（`Prefab::revert`） | 已实现 |
| 场景紧凑序列化（实例写成 prefab 引用） | 已实现 |
| 运行时变体（Prefab Variant） | 已实现（`.geprefabvariant` + Hierarchy 右键“创建变体”） |

---

## 4. 资源管线

### 4.1 资源管理器

| 功能 | 状态 |
|---|---|
| `AssetManager` 缓存 mesh/texture/material | 已实现 |
| `res:/` 路径虚拟化 | 已实现 |
| 资源引用计数 | 已实现（`AssetHandle<T>` + `std::shared_ptr<Asset>` 共享持有） |
| 异步加载 | 已实现（`AsyncLoader` 线程池 + `AssetManager::load_async`） |
| LRU 卸载 | 已实现（按最大缓存数量 / 最大内存用量驱逐，外部仍持有则保留） |
| 资源导入设置 `.gimport` | 已实现（`editor/import/gimport_settings.*`，Project 面板双击编辑） |
| 资源包 `.gpack` | 已实现（`resources/gpack_bundle.*` + `AssetManager::mount_bundle`） |

### 4.2 模型加载

| 功能 | 状态 |
|---|---|
| OBJ 加载器（含 MTL：Kd/Ke/d/Tr/Ns/贴图） | 已实现 |
| FBX 加载器 | 已实现（Assimp） |
| glTF 2.0 加载器 | 已实现（Assimp） |
| Assimp 集成（OBJ/FBX/glTF/DAE/PLY/STL + 材质提取） | 已实现 |
| 骨骼动画 | 已实现（CPU 插值 + GPU skinning + SkinnedMeshRenderer；状态机未实现） |

### 4.3 纹理与材质

| 功能 | 状态 |
|---|---|
| PNG/JPG/BMP 加载（stb_image） | 已实现（路径统一按 UTF-8 处理，支持中文文件名） |
| 法线/roughness/metallic/ao/emissive 贴图 | 已实现 |
| 立方体贴图（cubemap，天空盒） | 已实现 |
| 纹理压缩（BC1~BC7/ASTC/ETC2） | 已实现（DDS/KTX 加载 + GL/VK 上传） |
| Mipmap 自动生成 | 已实现（2D 纹理，cubemap 暂不支持） |
| HDR/EXR 环境贴图 | 已实现（通过 `RenderPipeline::set_environment_hdr` 供 IBL 使用） |
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
| 稳定的刚体旋转/角速度 | 已实现 |
| 连续碰撞检测（CCD） | 未实现 |
| 射线检测（Physics.Raycast） | 已实现 |
| 角色控制器 | 已实现（`CharacterController3D`：移动、跳跃、坡度限制、台阶抬升） |
| 关节系统（Hinge/Fixed/Spring/Distance） | 已实现（`Joint3D`，基于 Jolt） |
| Jolt Physics 集成 | 已实现（`GRYCE_HAS_JOLT`），默认优先，不可用时 fallback builtin 并告警 |

### 5.2 2D 物理

| 功能 | 状态 |
|---|---|
| 基础 AABB 碰撞 | 已实现 |
| 2D 刚体/静态体/碰撞盒 | 已实现 |
| 2D 圆形/多边形碰撞体 | 已实现（`CircleCollider2D`） |
| 2D 关节与平台碰撞 | 已实现（`Joint2D`：Distance/Spring，基于 Box2D） |
| 2D 角色控制器 | 已实现（`CharacterController2D`：移动、跳跃、坡度限制、台阶抬升） |
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
| ImGui 样式统一与主题文件 | 已实现（Fluent Design 深色/浅色 + 强调色 + 圆角/阴影，持久化到 `editor_theme.json`） |
| ImGui 字体资源统一走引擎字体系统 | 部分实现（`File > Settings` 可切换自定义字体并运行时热重载；但编辑器仍使用 ImGui 自带 ImFontAtlas，尚未复用 `core/render/font_atlas.cpp`） |

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
| 脚本语言 | 已选型：Lua（计划通过 sol2/LuaJIT 集成） |
| C++ 组件暴露到脚本 | 未实现 |
| 脚本生命周期回调 | 未实现 |
| 脚本热重载 | 未实现 |
| Visual Scripting | 未实现 |

**选型说明**：Lua 在嵌入体积（~200 KB）、C/C++ 互操作、热重载速度与 GC 可控性上均优于 Python/JS/TS，适合作为引擎脚本层；性能敏感路径继续由 C++ 实现并暴露 API。

---

## 9. 编辑器

| 功能 | 状态 |
|---|---|
| Docking 布局 + 面板管理框架 | 已实现 |
| 场景视图（Scene View） | 已实现（自由飞行相机 + F 聚焦 + 网格线 + ImGuizmo） |
| 游戏视图（Game View） | 已实现（独立渲染管线 + 主摄像机视角 + Viewport/Game 标签页） |
| 层级面板（Hierarchy） | 已实现（Entity 树、增删、拖拽换父、Prefab 标记、延迟删除、合成根顶行显示场景名） |
| Hierarchy 右键菜单与全局快捷键 | 已实现（新建/Cut/Copy/Paste/Duplicate/Rename/Focus/Prefab/Delete；Ctrl+X/C/V/D、F2、Del 注册于 ShortcutManager） |
| Create Entity 对话框（Godot 风格） | 已实现（`editor/ui/create_entity_dialog.*`：收藏/最近/搜索/过滤/描述，持久化到 `create_entity_dialog.json`） |
| File Explorer 右键菜单 | 已实现（新建文件夹/场景/材质、重命名、删除确认、复制路径） |
| Inspector 面板 | 已实现（反射自动生成字段编辑、enum 下拉、只读灰显） |
| 项目面板（Project） | 已实现（目录树、资源图标、双击加载、拖放） |
| 控制台面板（Console） | 已实现（日志过滤、颜色区分、自动滚动、点击定位） |
| 动画编辑器 | 已实现（剪辑选择、播放/暂停/循环/速度、时间滑块） |
| 材质编辑器 | 已实现（PBR 参数、贴图槽、物理属性、保存后即时 GPU 上传） |
| 地形编辑器 | 已实现（基础高度图编辑 + MeshRenderer 导出；完整 Terrain 渲染/LOD 留 M5） |
| 编辑器设置保存 | 已实现（`imgui.ini` 布局 + `editor_theme.json` / `editor_settings.json` 主题/语言/VSync/快捷键） |
| Settings 窗口分区 | 已实现（Theme / Appliance 语言 / Editor：VSync 持久化 + 场景自动保存间隔（分钟，0=关）/ Shortcuts：按键捕获改绑、冲突检测、重置，持久化到 `editor_settings.json` 的 `shortcuts` 组） |
| 启动时应用 VSync | 已实现 |
| 场景自动保存 | 已实现（按间隔保存脏场景，Play Mode 下跳过） |
| Project Settings 窗口 | 已实现（File > Project Settings：渲染 API（DX 显示为预留）+ Render Quality：shadow map 尺寸/bias/area、环境光、HDR、tone map、exposure、IBL 强度；持久化到 `project_settings.json` 的 `graphics` 组，重启生效） |
| 快捷键体系 | 已实现（Ctrl+S/Z/Y、Delete、F、Ctrl+P Play Mode、Ctrl+X/C/V/D、F2，支持改绑） |
| 命令行参数（CLI） | 已实现（`--vulkan`（默认）/`--opengl`/`--vulkan-validation`、`--scene`、`--record`、`--camera`、`--headless` 等） | 详见 `docs/CLI.md` |
| Undo/Redo | 已实现（属性修改、增删实体、Transform） |

---

## 10. 演示程序

### 10.1 3D 综合演示（3dtest）

| 功能 | 状态 |
|---|---|
| Cube + 地面 + PBR 材质 | 已实现 |
| 天空盒（程序化渐变天空 cubemap） | 已实现 |
| 多光源展示（方向光 + 点光 + 聚光） | 已实现 |
| 材质展示（自发光、半透明玻璃、UV 平铺） | 已实现 |
| FPS 相机（WASD、鼠标、Space/Ctrl） | 已实现 |
| 重力枪（鼠标左键拖拽） | 已实现 |
| 场景重置（R 键） | 已实现 |
| 材质预设与 ImGui 材质面板 | 已实现 |
| 3D 碎裂（F2） | 已实现 |
| 场景保存（F3） | 已实现 |
| Hinge 关节链 | 已实现 |
| 角色控制器（DemoCharacter，方向键移动，右 Shift 跳跃） | 已实现 |
| 音频源/监听器 | 已实现 |

### 10.2 2D 综合演示（gt2dDemo）

| 功能 | 状态 |
|---|---|
| 平台跑酷基础 | 已实现 |
| 完整关卡（平台、坑、敌人、金币） | 已实现 |
| 玩家角色（移动、跳跃、射击、受击、死亡、重生） | 已实现 |
| 敌人 AI（巡逻、踩踏死亡） | 已实现 |
| 瓦片地图（碰撞、光照、阴影） | 已实现 |
| 2D 光照（环境光、方向光、聚光灯、点光源） | 已实现 |
| 粒子特效（跳跃尘土、命中爆炸） | 已实现 |
| 音效（跳跃、金币、踩踏、受击） | 已实现 |
| UI（分数、金币、生命、FPS、提示） | 已实现 |
| 相机跟随与平滑插值 | 已实现 |
| 2D 形状展示（矩形、圆、三角、六边形） | 已实现 |
| DistanceJoint 吊桥 | 已实现 |
| 重力预设切换（8 大行星） | 已实现 |

### 10.3 独立功能演示（examples/）

| 演示 | 覆盖功能 | 状态 |
|---|---|---|
| `demo_sprite2d` | Sprite2D 贴图/纯色精灵 | 已实现 |
| `demo_shapes2d` | Circle、Polygon 形状 | 已实现 |
| `demo_lighting2d` | 2D 点光源 | 已实现 |
| `demo_tilemap2d` | 程序化瓦片地图 | 已实现 |
| `demo_particles2d` | 粒子发射器、爆发模式 | 已实现 |
| `demo_physics2d` | 2D 刚体、碰撞、堆叠 | 已实现 |
| `demo_character2d` | 2D 角色控制器 | 已实现 |
| `demo_joints2d` | 2D 距离关节 | 已实现 |
| `demo_physics3d` | 3D 刚体、球体、发射 | 已实现 |
| `demo_character3d` | 3D 角色控制器 | 已实现 |
| `demo_joints3d` | 3D Hinge 关节链 | 已实现 |
| `demo_fracture` | 可破坏体碎裂 | 已实现 |
| `demo_lighting3d` | 3D 方向光、点光源 | 已实现 |
| `demo_audio3d` | 3D 空间音频 | 已实现 |
| `demo_scene_serializer` | 场景保存/加载 | 已实现 |

---

## 11. 性能与优化

| 功能 | 状态 |
|---|---|
| Vulkan/OpenGL 批处理 | 已实现 |
| 异步日志（AsyncLogger） | 已实现（`core/utils/glog/glog_lib.*`：`log()` 入队、worker 线程写出；`GLog` 自动包装默认/自定义 logger；`MemoryLogSink::from_glog()` 穿透包装；`flush()` 等待排空） |
| 每帧热路径日志降级 | 已实现（降为 `GLOG_DEBUG`） |
| `GL_CHECK_ERROR` Release 编译剔除 | 已实现（`NDEBUG` 下为空操作） |
| DrawItem 向量跨帧复用 | 已实现 |
| 连续相同材质绑定跳过 | 已实现 |
| Vulkan/OpenGL NDC z 双重映射修复 | 已实现（此前导致全场景误判为阴影） |
| GL `set_swap_interval` 无当前上下文防护 | 已实现 |
| GPU profiling（Nsight/RenderDoc 标记） | 未实现 |
| CPU profiling（Tracy/自带 profiler） | 未实现 |
| 视锥剔除 | 部分实现（RenderPipeline 已有局部包围球 + camera frustum 判断） |
| 遮挡剔除 | 未实现 |
| GPU driven rendering | 未实现 |
| 纹理流送 | 未实现 |
| LOD 系统 | 未实现 |
| GPU Instancing | 未实现（规划：按 mesh+material+two_sided 分桶，减少 Draw Call） |
| 共享可见 entity 列表 | 未实现（规划：shadow/forward pass 复用同一份 VisibleItem 列表） |
| Material/Shader 状态缓存 | 部分实现（连续相同材质绑定跳过） |
| 包围球脏标记 | 未实现（规划：Transform 版本号 + 缓存世界空间包围球） |

---

## 12. 文档

| 文档 | 状态 |
|---|---|
| 架构文档（ARCHITECTURE.md） | 已完成 |
| 资源路径规范（PROJECT_LAYOUT.md） | 已完成 |
| 工程状态报告（STATUS.md） | 已完成 |
| Core API 规范（CORE_API.md） | 已完成 |
| 路线图（TODO.md） | 已完成 |
| 命令行参数参考（CLI.md） | 已完成 |
| API 文档（doxygen） | 未生成 |
| 用户手册 / 快速入门 | 部分实现（README 快速开始已覆盖） |
| 着色器编写指南 | 未编写 |

---

## 13. 已知问题与调试记录

> 本节记录 MSVC / Windows 平台下遇到的典型崩溃与渲染异常，以及排查结论。

### 13.1 MSVC 下 `unique_ptr<IImGuiBackend>` 返回时 `this == nullptr` 崩溃

**现象**
- 调试器报告：写入访问冲突，`this == nullptr`。
- 调用栈停在 `std::_Compressed_pair<...>` 的构造函数（`xmemory` 第 1556 行），触发路径为 `GLBackend::create_imgui_backend()` 返回 `std::make_unique<GLImGuiBackend>()`，随后进入 `unique_ptr<IImGuiBackend>` 的转换构造函数。
- `GLImGuiBackend` 本身没有自定义构造函数，崩溃发生在对象构造 / `unique_ptr` 转换阶段，尚未执行 `init()`。

**根因**
`IImGuiBackend` 类以值形式通过 `std::unique_ptr<IImGuiBackend>` 跨 DLL（`gryce_core` → `gryce_engine` / 编辑器）返回。若接口类未导出，MSVC 在模块间使用不同的 vtable / 析构函数布局，导致返回值槽位地址为 `null`，从而在 `unique_ptr` 内部写入成员时触发 `this == nullptr`。

**修复**
- `core/render/imgui_backend.h`：`class IImGuiBackend` 加上 `GRYCE_API`。
- `core/render/render.h`：`class IRenderBackend` 同样加上 `GRYCE_API`（该接口也通过 `unique_ptr` 跨 DLL 传递）。
- 确保 `gryce_core` 的导出宏在 Windows / MSVC 下正确定义并链接到使用方。

**验证**
- `gryce_core` 与 `gryce_engine` 编译、链接通过。
- 运行时 `create_imgui_backend()` 不再在 `unique_ptr` 转换处崩溃。

### 13.2 Vulkan 阴影/光照异常（阴影方向偏移、边缘黑边、混合错误）

**现象**
- Vulkan 后端下 3D 场景阴影整体方向偏移、光照明暗区域与 OpenGL 不一致。
- 阴影贴图边缘出现明显黑边（超出 shadow map 范围的部分被错误判定为阴影）。
- 部分不透明物体看起来半透明，PBR 高光/环境光混合异常。

**根因**
1. `VulkanShader::create_pipeline` 中 `blendEnable` 被条件错误地设为 `VK_TRUE`，导致大量不透明管线强制开启 Alpha Blend，破坏光照累积。
2. 深度阴影贴图 sampler 使用 `VK_BORDER_COLOR_INT_OPAQUE_BLACK`，`ClampToBorder` 时边界深度为 0.0，被 `LESS_OR_EQUAL` 判定为处于阴影，产生黑边。
3. Vulkan NDC 的 Y 轴与 OpenGL 相反，而 `vulkan_pbr.frag` 的 `shadow_calculation` 未对 `proj_coords.y` 做翻转，导致采样 shadow map 的 UV 上下颠倒。
4. `vk_backend` 硬编码 `max_push_constant_size = 128`，但现代 NVIDIA GPU（如 RTX 5070）实际支持 256B；硬编码值不会直接导致崩溃，但会虚假限制 push constant 可用空间。

**修复**
- `core/render/vulkan/vk_shader.cpp`：不透明管线统一设置 `blendEnable = VK_FALSE`；透明材质未来通过专用 pipeline 或 `VK_EXT_extended_dynamic_state3` 动态混合支持。
- `core/render/vulkan/vk_texture.cpp`：深度纹理 sampler 的 `borderColor` 改为 `VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE`，使边界深度为 1.0（最远），不被判定为阴影。
- `examples/3dtest/shaders/vulkan_pbr.frag` 与 `examples/gt2dDemo/shaders/vulkan_pbr.frag`：在 `shadow_calculation` 中加入 `proj_coords.y = 1.0 - proj_coords.y`。
- `core/render/vulkan/vk_device.cpp/.h`：读取 `physicalDeviceProperties.limits.maxPushConstantsSize`。
- `core/render/vulkan/vk_backend.cpp`：使用 `device_.max_push_constants_size()` 替代硬编码 128。

**验证**
- `gryce-engine.exe --vulkan --headless --auto-close 2` 可正常运行退出，无 Vulkan 错误。（历史记录：当时通过截图参数验证，该参数现已移除。）
- 设备日志正确报告 `max_push_constants=256`（RTX 5070 Laptop）。
- 对应 SPIR-V 已本地重新生成；`*.spv` 在 `.gitignore` 中，构建或运行前需用 `glslangValidator -V` 重新编译着色器源。

### 13.3 OpenGL 编辑器 3D 场景视口上下颠倒

**现象**
- OpenGL 后端下，编辑器 Viewport 中的 3D 场景与 Game View 中的方向正好相反（例如默认进入时地面出现在视口上方，按 F 聚焦 Ground 后暂时正常）。
- 2D、物理调试绘制方向均正常；Vulkan 后端下 3D 方向也正常。

**根因**
- 第一阶段怀疑是 `ViewportPanel` / `GameViewPanel` 对 OpenGL 端做了多余的 V 轴翻转，但移除后问题并未解决，且会破坏 2D/物理的显示方向。
- 第二阶段怀疑是 `math::Camera` 与 `math::Quaternionf::from_euler` 的欧拉角约定不一致，因此用 `camera.forward()/up()/right()` 直接构造四元数、用 `rotation.rotate_vector(Vector3f::forward())` 还原 `pitch`/`yaw`。但用户反馈“默认进去仍反，聚焦 Ground 正常、不聚焦又反”，说明 Game View 与 Viewport 的相机朝向仍是反向。
- 最终根因：引擎内部存在两套四元数旋转约定：
  - `Quaternionf::rotate_vector(v)` 计算 `q * v * q^-1`（标准数学约定）；
  - `Transform::local_matrix()` 使用 `Matrix4f::from_quaternion(q)`，即 `q.to_matrix()`，其矩阵乘法等价于 `q^-1 * v * q`（与 `rotate_vector` 方向相反）。
- 因此，把 `Transform::rotation` 读回 `math::Camera` 时，若用 `rotation.rotate_vector(Vector3f::forward())` 会得到与渲染管线（Game View）相反的前向向量，导致 Viewport 相机与 Game View 相机朝向相反。

**修复**
- `editor/editor_app.cpp` 的 `apply_quaternion_to_camera`：改用 `rotation.conjugate().rotate_vector(Vector3f::forward())` 还原世界前向，使其与 `Transform::local_matrix()` / 渲染管线约定保持一致。
- `camera_rotation_to_quaternion`（`camera -> Transform::rotation`）本身已按 `to_matrix` 约定构造，无需修改。
- 保留此前新增的 `sync_editor_to_scene_camera`，在场景加载/热重载/Play Mode 恢复后把编辑器相机同步到场景 `MainCamera` 的 Transform，避免加载已有场景时起始位置/方向不一致。
- 保留 `editor/editor_camera.cpp` 中 `focus_on_bounds` 的斜上方 45° 聚焦逻辑，作为兜底体验优化。
- `examples/3dtest/3dtest.cpp` 只把相机写入场景，不从中读回，因此不受影响；为保持一致性仍使用 `camera_rotation_to_quaternion`。

**验证**
- 重新编译后，OpenGL 后端下编辑器 Viewport 与 Game View 的 3D 场景方向一致，且 2D / 物理调试绘制方向不受影响。
- 按 F 聚焦 Ground 后视角不再“临时反转”，离开聚焦状态也能保持与 Game View 同向。
