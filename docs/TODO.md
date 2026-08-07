# Gryce Engine — Roadmap to a Unity-class Engine

> 本文件记录从当前版本到"类似 Unity 的通用游戏引擎"所需的全部任务。状态分为：
> - **已完成**（本轮已合并）
> - **进行中**（已开工，未完成）
> - **待实现**（按里程碑排序，见里程碑总览）
> - **远期目标**
>
> 排序原则：按必要性和发展顺序。**第一大目标是拥有可用的编辑器（M1）**——
> 编辑器是引擎生产力的放大器，之后的渲染补完、玩法系统都能在编辑器里
> 即时验证，开发效率远高于纯代码迭代。
>
> 近期质量基线：渲染线程生命周期/命令队列/资源池 generation/GL-VK 线程模型
> 已全面修复加固（6 高 + 8 中 + 低危清零），新功能必须沿用 alive_token、
> 句柄 + generation 校验、主线程重建 pause/resume 这套模式。

---

## 里程碑总览

| 阶段 | 目标 | 关键产出 |
|---|---|---|
| M0 当前 | 核心渲染 + ECS + 物理 + 场景系统 + 质量加固 | 已可跑 3D/2D 综合演示，渲染线程模型稳固 |
| **M1 编辑器 MVP** | **可视化编辑器：场景编辑、属性编辑、资源浏览、Play Mode** | **非纯代码方式搭建和调试场景** |
| M2 编辑器完全体 + 内容管线 | 材质编辑器、导入设置、资源包、Prefab 编辑集成 | 完整内容生产工作流 |
| M3 渲染补完 | IBL、后处理栈、CSM、抗锯齿、材质预设库 | 可制作中等复杂度、画质合格的 3D 游戏 |
| M4 玩法系统 | 脚本、动画状态机、运行时 UI、AI、事件总线 | 可制作完整游戏逻辑 |
| M5 生产级渲染 | SSAO/SSR、TAA、GPU 粒子、地形、LOD | 接近商业引擎画质下限 |
| M6 平台与规模化 | 移动端、网络多人、性能分析器、发布管线 | 可发布商业产品 |

> 与原路线图（M1 核心补完 → M4 编辑器）的差异：编辑器提前为第一大目标。
> 骨骼动画（CPU 侧 + GPU skinning + SkinnedMeshRenderer + AnimatorSystem）已完成，
> E0 收尾结束，动画状态机/混合排入 M4。

---

## M1 编辑器 MVP — 第一大目标详表

> 依赖关系决定实施顺序。E0 是收尾项；E1 是后续所有面板的前置；
> E2~E5 是 MVP 闭环；E6 让编辑器真正可用。

### E0 收尾（已完成）

| 任务 | 状态 | 说明 |
|---|---|---|
| 骨骼动画 CPU 侧（Skeleton / AnimationClip / Assimp skin 导入） | **已完成** | `core/animation/` + `import_skinned`，117 测试通过 |
| GPU Skinning + SkinnedMeshRenderer + AnimatorSystem | **已完成** | 顶点布局 location 5/6（bone ids/weights），GL/VK 蒙皮 PBR shader，palette 经 `set_uniform_mat4_array` 渲染线程上传 |

### E1 编辑器基础设施（前置依赖）

| 任务 | 状态 | 说明 |
|---|---|---|
| 组件反射系统（字段注册 / 类型信息 / 读写访问） | **已完成** | `core/reflection/`，宏注册 + 类型擦除读写，12 类型已注册，9 测试通过 |
| ImGui Docking 布局 + 面板管理框架 | **已完成** | `editor/`：EditorPanel 基类 + PanelManager（DockSpace over viewport + DockBuilder 默认布局），布局持久化到 `editor/project/editor_imgui.ini` |
| 编辑器摄像机（Scene View 自由飞行相机） | **已完成** | `editor/editor_camera.*`：右键视角 + WASD/QE + 滚轮调速 + Shift 加速，F 按选中实体包围球聚焦；仅 Viewport 悬停时响应 |
| GLOG 捕获到内存 buffer | **已完成** | `core/utils/glog/`：MemoryLogSink（tee 模式环形缓冲，容量 1000，线程安全），Console 面板级别过滤 + 自动滚动 |
| 渲染到纹理 + 视口嵌入 ImGui | **已完成** | RenderPipeline 视口离屏输出（tonemap → 独立 FBO），Viewport 面板 ImGui::Image 嵌入；面板尺寸防抖同步渲染目标；OpenGL/Vulkan 双后端均支持纹理 ID 转换 |

### E2 场景编辑核心（MVP 闭环）

| 任务 | 状态 | Unity 对应 |
|---|---|---|
| Hierarchy 面板（Entity 树、增删、拖拽换父、Prefab 标记） | **已完成** | 右键菜单（创建/重命名/删除）、拖拽换父（含拖回根级、环检测）、UUID 弱引用选中、[P] Prefab 标记；删除/换父延迟到帧末执行防迭代器失效 |
| Inspector 面板（基于反射自动生成组件属性编辑） | **已完成** | 反射字段自动分派控件（Drag/Slider/Checkbox/InputText/ColorEdit/Combo），只读灰显，enabled 勾选；enum 字段走 `GRYCE_REFLECT_FIELD_ENUM` + `ImGui::Combo` 下拉编辑，标签由 `locales/*.json` 的 `inspector.enum.{Type}.{field}` 提供 |
| Scene View（3D 视口 + 编辑相机 + 网格线） | **已完成** | `RenderPipeline` 中新增 `create_grid_mesh` / `render_grid`；XZ 平面网格 + 主次线 + 渐隐；Debug 面板可开关 |
| 点选拾取（raycast 选中 Entity） | **已完成** | `core/math/ray.h`（NDC 反投影 + slab AABB），逐 mesh 世界 AABB 求交取最近命中，不依赖碰撞体；7 测试通过 |
| Transform Gizmo（移动/旋转/缩放手柄） | **已完成** | ImGuizmo 集成（third_party/imguizmo），W/E/R 切换，gizmo 激活时屏蔽相机与拾取；TRS 分解走 from_rotation_matrix（2 测试通过） |
| 场景保存/加载挂到编辑器菜单 | **已完成** | File 菜单：Save（Ctrl+S）/ Save As / Open，路径弹窗走 SceneSerializer；保存后刷新 mtime 缓存防误触发热重载 |

### E3 资源与项目面板

| 任务 | 状态 | Unity 对应 |
|---|---|---|
| Project / Content Browser（目录树、资源图标、双击加载） | **已完成** | `editor/panels/project_panel.*`，图标化文件列表 + 路径栏/进入目录 |
| 拖放资源到场景/Inspector（纹理→材质、模型→场景、Prefab→场景） | **已完成** | Project 面板作为 drag source；Hierarchy/Viewport/Inspector 作为 drop target；支持 .obj/.fbx/.gltf/.glb 实例化、纹理赋给 MeshRenderer、.gesc 打开场景 |
| `.gimport` 导入设置（模型缩放、碰撞体、刚体、物理材质） | **已完成** | `editor/import/gimport_settings.*` + `editor/ui/gimport_editor_window.*`；Project 面板双击 .gimport 编辑；实例化模型时自动应用 |

### E4 运行与调试

| 任务 | 状态 | Unity 对应 |
|---|---|---|
| Game View（运行时画面嵌入编辑器） | **已完成** | Game View：独立 RenderPipeline + 主摄像机构建 game camera；Viewport/Game 标签页共享中心区域；后台标签页不渲染 |
| Play Mode（进入时场景快照、退出时恢复） | **已完成** | Play Mode |
| Console 面板（日志过滤、点击定位） | **已完成** | `editor/panels/console_panel.*`：级别过滤 + 自动滚动 + 颜色区分；点击日志通过 `vscode://file/...` 打开源码位置 |

### E5 可用性收尾

| 任务 | 状态 | 说明 |
|---|---|---|
| 编辑器布局/设置持久化 | **已完成** | `imgui.ini` 已持久化布局；`File > Settings` 窗口已持久化主题与语言到 `editor_theme.json` / `editor_settings.json` |
| 主题文件与样式统一 | **已完成** | Fluent Design 深色/浅色 + 强调色 + 自定义字体，配置持久化到 `editor_theme.json`；浅色主题下 Console 日志颜色自动取反为深色文字 |
| Settings 窗口（File > Settings） | **已完成** | 左侧 Theme / Appliance 栏目；Theme 栏目管理外观；Appliance 栏目管理语言 |
| 编辑器多语言本地化 | **已完成** | `editor/localization/` 单例 + `locales/{en,zh,ja}.json`；支持中文/英文/日文，运行时热重载，所有面板/菜单/弹窗走 `tr()` 翻译 |
| 快捷键体系（保存/撤销/删除/聚焦） | **已完成** | `ShortcutManager` + `KeyCombo`；Ctrl+S 保存、Ctrl+Z/Y Undo/Redo、Delete 删除、F 聚焦、Ctrl+P Play Mode |
| Undo/Redo（命令模式，最小覆盖：属性修改、增删实体、Transform） | **已完成** | `CommandStack` 双栈 + `ComponentFieldCommand` / `Entity*Command`；Inspector、Hierarchy、Viewport Gizmo 已接入 |

---

## 1. ECS / 场景系统

| 任务 | 状态 | 里程碑 | 说明 / Unity 对应 |
|---|---|---|---|
| Prefab 完整实现（覆盖参数、嵌套、revert、场景紧凑引用） | **已完成** | — | `PrefabInstance` + `Prefab::revert` |
| 场景单根节点（Node 架构） | **已完成** | — | 合成根 Entity（`Scene::root()`）；`.gesc` 版本 2（v1 兼容）；Hierarchy 顶行显示场景名；Undo 按"根的子节点"处理根级操作 |
| 2D 父链变换 | **已完成** | — | `world_transform_2d()` 组合祖先变换；`Node2D::top_level` 脱离父链；`z_index` 参与排序；2D 物理与 2D Gizmo 使用世界空间 |
| 2D / 3D 场景拆分 | 待实现 | M4 | 当前 2D/3D 组件共存于同一场景，计划拆分独立场景类型 |
| 组件反射 Inspector 生成 | **已完成** | **M1-E1** | Editor Inspector 前置 |
| Prefab 编辑器集成（编辑器内创建/应用/还原） | **已完成** | M2 | Hierarchy 右键菜单：Create/Apply/Revert Prefab |
| Prefab Variant（覆盖持久化 + 属性优先级） | **已完成** | M2 | `.geprefabvariant` + Hierarchy 右键"创建变体" |
| 场景流送性能优化（异步加载、大世界分块） | 待实现 | M4 | `SceneManager.LoadSceneAsync` |
| ECS Archetype / Chunk 内存布局 | 远期目标 | — | Unity ECS / Unreal Mass |
| Job System（多线程批处理组件） | 远期目标 | — | `Unity.Jobs` |
| Burst-like SIMD 编译优化 | 远期目标 | — | `Unity.Burst` |

---

## 2. 渲染（RHI + 管线）

### 2.1 渲染补完（M3）

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| DDS/KTX 压缩纹理（BC1~BC7 / ETC2 / ASTC） | **已完成** | — | GL + VK 双后端 |
| 渲染后端分层（Vulkan 默认 / OpenGL 兼容 / DX 预留） | **已完成** | — | `RenderAPI` 枚举 + Project Settings 渲染 API 下拉 + Render Quality 区（`project_settings.json` `graphics` 组） |
| 阴影系统加固 | **已完成** | — | 光空间贴合相机视锥 + 纹素对齐 + 深度延伸 50；着色器边缘 5% 淡出；自适应 bias + Vulkan slope-scaled depth bias；NDC z 双重映射修复 |
| HDR/EXR 环境贴图加载 | **已完成** | — | `RenderPipeline::set_environment_hdr`，CPU 端生成 IBL |
| IBL（Image-Based Lighting） | **已完成** | — | irradiance + prefilter + BRDF LUT；OpenGL/Vulkan PBR 与 Skinned PBR |
| Cubemap Mipmap 生成 | 待实现 | M3 | 当前 cubemap 强制 1 mip |
| 材质预设库（MaterialLibrary） | 待实现 | M3 | 内置 metal/plastic/glass/wood，编辑器一键应用 |
| 后处理栈（Bloom / Color Grading LUT / Volume 配置） | 部分完成 | M3 | Bloom 已实现（OpenGL / Vulkan 2D）；Color Grading LUT、Volume 配置待实现 |
| 屏幕空间雾 / Volumetric Fog | 待实现 | M3 | 氛围渲染 |
| 色调映射扩展（Filmic、Uncharted2） | 待实现 | M3 | 当前 ACES/Reinhard |
| 级联阴影（CSM） | 待实现 | M3 | Cascaded Shadow Maps |
| TAA / FXAA / SMAA | 待实现 | M3 | 抗锯齿 |

### 2.2 生产级渲染（M5）

| 任务 | 状态 | 说明 / Unity 对应 |
|---|---|---|
| SSAO / SSR | 待实现 | Screen Space Ambient Occlusion / Reflections |
| Motion Blur / Depth of Field | 待实现 | 镜头效果 |
| GPU Particle System | 待实现 | Unity VFX Graph 简化版 |
| Decal / Projector | 待实现 | 贴花系统 |
| Terrain（高度图、LOD、刷草） | 待实现 | Unity Terrain |
| Mesh LOD / HLOD | 待实现 | LOD Group |
| GPU-Driven Rendering / Indirect Draw | 远期目标 | 海量物体；GPU instancing（A1）已延期，随本项一起做 |
| D3D12 / Metal 后端 | 远期目标 | 多平台图形 API；DX11/DX12 枚举值已预留（`create_render_backend` 返回 `nullptr`），实现未开始 |
| Ray Tracing（RTX/DXR/Metal RT） | 远期目标 | 光追反射/阴影 |
| Virtual Shadow Maps / GPU Culling | 远期目标 | 开放世界阴影 |

---

## 3. 动画系统

| 任务 | 状态 | 里程碑 | 说明 / Unity 对应 |
|---|---|---|---|
| 骨骼动画 CPU 侧（数据结构 + 插值 + pose 求值） | **已完成** | — | `Skeleton` / `AnimationClip` / `evaluate_skin_palette` |
| Assimp 骨骼数据导入 | **已完成** | — | `.fbx`/`.gltf` skin/cluster，含集成测试 |
| GPU Skinning + SkinnedMeshRenderer + AnimatorSystem | **已完成** | **M1-E0** | 顶点着色器 LBS 蒙皮（GL/VK），palette 渲染线程上传，128 骨上限 |
| Animator Controller / 状态机 | 待实现 | M4 | Mecanim 简化版 |
| Blend Tree / 1D/2D 混合 | 待实现 | M4 | 移动混合 |
| 动画事件（关键帧回调） | 待实现 | M4 | AnimationEvent |
| 动画编辑器（关键帧剪辑） | **已完成** | M2 | `AnimationEditorWindow`：剪辑选择、播放/暂停/循环/速度、时间滑块 |
| Inverse Kinematics（IK） | 待实现 | M5 | 足部/手部 IK |
| 动画重定向（Retargeting） | 远期目标 | — | 不同骨架复用动画 |
| Timeline / 剧情动画 | 远期目标 | — | Timeline 简化版 |

---

## 4. 资源管线

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 纹理压缩（DDS/KTX） | **已完成** | — | 含 GL/VK 上传 |
| `.gimport` 导入设置 | **已完成** | **M1-E3** | 纹理/模型导入参数（编辑器最小版先行） |
| 内容浏览器（Content Browser） | **已完成** | **M1-E3** | 编辑器资源面板 |
| 资源引用计数 + LRU 卸载 | **已完成** | M2 | `Asset::memory_size()` + `AssetManager` 计数/内存限制 + LRU 驱逐 |
| 资源包 `.gpack` / AssetBundle | **已完成** | M2 | `GPackReader/Writer` + `AssetManager::mount_bundle` |
| 模型 LOD / 碰撞体自动生成 | 待实现 | M2 | 导入时构建 LOD 和 convex hull |
| 字体 SDF 生成 | 待实现 | M4 | 高清字体渲染（运行时 UI 前置） |
| 视频纹理 / Streaming | 远期目标 | — | 视频贴图 |

---

## 5. 物理系统

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 屏幕点选 raycast（编辑器拾取） | **已完成** | **M1-E2** | 物理 raycast 已有，需屏幕到射线封装 |
| 连续碰撞检测（CCD）配置 | 待实现 | M4 | 高速物体防穿透 |
| Ragdoll / 物理布娃娃 | 待实现 | M4 | 角色死亡/击飞 |
| 物理材质编辑器 | **已完成** | M2 | Inspector 中 PhysicalMaterial 预设下拉 + 参数编辑 |
| Cloth / Soft Body | 远期目标 | — | 布料、绳索 |
| Vehicle Physics | 远期目标 | — | 车辆悬挂、轮胎 |
| 碎裂 Voronoi / 有限元 | 远期目标 | — | 真实破坏 |

---

## 6. 音频系统

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 音频资源格式完整测试（WAV/OGG/MP3） | 待实现 | M2 | miniaudio 已集成 |
| 3D 空间音频 | 待实现 | M4 | 衰减、多普勒、HRTF |
| 音频总线（Master / SFX / Music / Voice） | 待实现 | M4 | 混音与快照 |
| DSP 效果器（Reverb、LowPass、Echo） | 待实现 | M4 | 环境混响 |
| 音频可视化 / Spectrum | 远期目标 | — | 频谱分析 |

---

## 7. 脚本与 gameplay

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 事件/消息总线 | 待实现 | M4 | 解耦系统通信 |
| 脚本语言选型与集成（Lua / C# / Python） | 待实现 | M4 | 推荐 Lua 或 C#；Node 架构（场景单根）已完成，脚本系统可在此基础上启动 |
| C++ 组件绑定到脚本 | 待实现 | M4 | 反射（M1-E1）可直接复用 |
| 脚本生命周期回调 | 待实现 | M4 | `Start` / `Update` / `OnCollisionEnter` |
| 脚本热重载 | 待实现 | M4 | 开发时快速迭代 |
| Visual Scripting（节点图） | 远期目标 | — | Blueprint 简化版 |
| 任务/成就系统 | 远期目标 | — | 游戏框架层 |

---

## 8. AI 系统

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| NavMesh 寻路 | 待实现 | M4 | Recast/Detour 集成 |
| Behavior Tree | 待实现 | M4 | 行为树编辑器 |
| 状态机（State Machine） | 待实现 | M4 | 敌人 AI |
| Steering Behaviors | 待实现 | M4 | 群体移动 |
| 寻路调试可视化 | 待实现 | M4 | 编辑器 Debug Draw（依赖 M1） |
| GOAP / HTN | 远期目标 | — | 高级决策 |

---

## 9. UI 系统

### 9.1 运行时 UI（M4）

| 任务 | 状态 | 说明 / Unity 对应 |
|---|---|---|
| UI Canvas / Panel / Button / Image / Slider / Dropdown | 待实现 | uGUI 简化版 |
| UI 事件系统（Raycast / EventSystem） | 待实现 | 点击/拖拽/滚动 |
| UI 布局系统（Anchor、LayoutGroup、Grid） | 待实现 | 自适应布局 |
| UI 动画与缓动 | 待实现 | 过渡动画 |
| SDF 字体 + 富文本 | 待实现 | 高清文字与颜色标签 |
| 多语言 / 国际化 | 远期目标 | 本地化 |

### 9.2 ImGui 编辑器层（M1）

| 任务 | 状态 | 说明 |
|---|---|---|
| 主题文件与样式统一 | **已完成** | **M1-E5**：Fluent Design 深色/浅色、强调色、圆角、阴影、自定义字体，持久化到 `editor_theme.json` |
| ImGui 字体走引擎 FontAtlas | 待实现 | M1-E5，统一字体管理 |

---

## 10. 编辑器（M1 主战场，详见顶部 M1 详表）

| 任务 | 状态 | 阶段 |
|---|---|---|
| 组件反射系统 | **已完成** | **M1-E1** |
| Docking 布局 + 面板框架 | **已完成** | **M1-E1** |
| 层级面板（Hierarchy） | **已完成** | **M1-E2** |
| Inspector 面板 | **已完成** | **M1-E2** |
| 场景视图（Scene View） | **已完成** | **M1-E2**：自由飞行相机 + F 聚焦 + Gizmo |
| 点选拾取 + Transform Gizmo | **已完成** | **M1-E2** |
| 项目面板（Project / Assets） | **已完成** | **M1-E3** |
| 游戏视图（Game View） | **已完成** | **M1-E4**：独立渲染管线 + 主摄像机视角 + Viewport/Game 标签页布局 |
| Play Mode | **已完成** | **M1-E4** |
| 控制台面板（Console） | **已完成** | **M1-E4**：过滤/颜色/自动滚动/点击定位完成 |
| 布局/设置持久化 + 快捷键 + Undo/Redo | **已完成** | **M1-E5** |
| Create Entity 对话框（Godot 风格） | **已完成** | 收藏/最近/搜索/过滤/描述；Hierarchy 右键"新建…"打开；持久化到 `create_entity_dialog.json` |
| Hierarchy 右键菜单 + 全局快捷键（Ctrl+X/C/V/D、F2、Del） | **已完成** | 新建/Cut/Copy/Paste/Duplicate/Rename/Focus/Prefab/Delete，注册于 ShortcutManager |
| File Explorer 右键菜单 | **已完成** | 新建文件夹/场景/材质、重命名、删除确认、复制路径 |
| Settings 窗口四分区（Theme / Appliance / Editor / Shortcuts） | **已完成** | Editor 区：VSync 持久化 + 自动保存间隔（Play Mode 跳过）；Shortcuts 区：按键捕获改绑 + 冲突检测 + 重置 |
| Project Settings 窗口（渲染 API + Render Quality） | **已完成** | `project_settings.json` `graphics` 组，启动时应用到两条编辑器管线，重启生效 |
| 材质编辑器 | **已完成** | M2，参数面板：PBR 参数、贴图槽、物理属性 |
| 动画编辑器 | **已完成** | M2，关键帧剪辑播放控制 |
| 地形编辑器 | **已完成** | M2，基础高度图编辑 + MeshRenderer 导出（完整 Terrain 渲染/LOD 留 M5） |
| Asset Store / 包管理器 | 远期目标 | 生态 |

---

## 11. 网络与多人（M6 / 远期）

| 任务 | 状态 | 说明 |
|---|---|---|
| 网络传输层（UDP + 可靠消息） | 远期目标 | ENET / GameNetworkingSockets |
| 客户端-服务器架构 | 远期目标 | 权威服务器 |
| 状态同步与快照插值 | 远期目标 | 多人同步 |
| 客户端预测与回滚 | 远期目标 | 射击/动作游戏 |
| Lobby / Matchmaking | 远期目标 | 房间匹配 |

---

## 12. 平台与发布（M6）

| 任务 | 状态 | 说明 |
|---|---|---|
| 发布包打包脚本 | 待实现 | 资源+可执行文件打包（可提前到 M2，低成本高收益） |
| Android / iOS 移植 | 远期目标 | 移动端图形后端 |
| 主机平台（Console） | 远期目标 | NDA 平台 |
| 安装程序 / 启动器 | 远期目标 | 自动更新 |
| Steam / 平台 SDK 集成 | 远期目标 | 成就/云存档 |

---

## 13. 性能与调优

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 异步日志（AsyncLogger） | **已完成** | — | `core/utils/glog/`：log() 入队 + worker 线程写出；GLog 自动包装 logger；flush() 等待排空 |
| 热路径性能批次 | **已完成** | — | 每帧日志降为 GLOG_DEBUG；GL_CHECK_ERROR Release 剔除；DrawItem 跨帧复用；相同材质绑定跳过；NDC z 双重映射修复；set_swap_interval 上下文防护 |
| GPU instancing（A1） | 延期 | — | 并入 GPU-Driven Rendering 一起做 |
| GPU Profiling（RenderDoc/Nsight 标记） | 待实现 | M2 | 渲染 Pass 标注（编辑器开发期间就需要） |
| 遮挡剔除 / 视锥剔除 | 待实现 | M3 | 减少 draw call |
| CPU Profiler / Tracy 集成 | 待实现 | M6 | 性能火焰图 |
| 内存分析器 | 待实现 | M6 | 资源占用 |
| GPU Culling | 远期目标 | — | 间接绘制 |
| 纹理流送 | 远期目标 | — | 大场景纹理管理 |

---

## 14. 文档与社区

| 任务 | 状态 | 里程碑 | 说明 |
|---|---|---|---|
| 着色器编写指南 | 待实现 | M3 | 双后端着色器规范（渲染补完时同步写） |
| 示例项目模板 | 待实现 | M4 | FPS/RPG/平台跳跃模板（用编辑器实际搭建，验证工作流） |
| API 文档（Doxygen） | 待实现 | M6 | 自动 API 生成 |
| 用户手册 / 快速入门 | 部分实现 | M6 | README 快速开始已覆盖，完整手册待编写 |
| 官方教程与视频 | 远期目标 | — | 社区建设 |

---

## 建议的下一步（按发展顺序）

1. ~~E0 收尾：GPU Skinning + SkinnedMeshRenderer~~（已完成：GL/VK 蒙皮管线 + AnimatorSystem + demo_skinned3d）。
2. ~~E1 组件反射系统~~（已完成：`core/reflection/`，宏注册 + 类型擦除，12 类型覆盖）。
3. ~~E1 编辑器骨架~~（已完成：`editor/` 独立目标 gryce.exe，Docking + 5 面板 + 编辑器相机 + Console 日志 + GL 视口纹理，134/134 测试通过）。
4. ~~E2 场景编辑闭环~~（已完成：Hierarchy 增删/拖拽换父/UUID 弱引用、Inspector 反射编辑、AABB 点选拾取、ImGuizmo 三模式 Gizmo、File 菜单保存/加载，143/143 测试通过；Scene View 网格线遗留）。
5. ~~E3 资源面板 + 拖放~~（已完成：Project 面板资源浏览 + 路径导航 + Hierarchy/Viewport/Inspector 拖放，支持模型/纹理/场景，143/143 测试通过）。
6. ~~E4 Play Mode~~（已完成：场景快照/恢复 + Play/Stop UI + 编辑状态切换 + CI 测试模式，运行验证通过）。
7. ~~E5 Fluent Design 主题系统~~（已完成：深色/浅色主题、强调色、圆角、阴影、自定义字体加载、View 菜单实时切换、配置持久化到 `editor_theme.json`，143/143 测试通过）。
8. ~~E5 Settings 窗口~~（已完成：`File > Settings` 入口、左侧 Theme / Appliance 栏目、Theme 管理外观、Appliance 管理语言、配置持久化到 `editor_settings.json`，143/143 测试通过）。
9. **E4 Game View + Console 点击定位**，**E5 布局完全持久化 + 快捷键体系 + Undo/Redo**（已完成，146/146 测试通过）。
10. **M2 编辑器完全体 + 内容管线**（已完成：材质/动画/地形/粒子编辑器、Prefab 编辑器集成、资源数据库、LRU 缓存、`.gpack` 资源包、物理材质编辑器、Prefab Variant，146/146 测试通过）。
11. **Node 架构 + 渲染后端分层 + 编辑器可用性 + 性能批次**（已完成：场景单根 Entity（`.gesc` v2）+ 2D 父链变换（`world_transform_2d` / `top_level` / `z_index`）；Vulkan 设为默认后端、OpenGL 降为兼容后端、DX11/12 预留；阴影贴合视锥 + 边缘淡出 + 自适应 bias；Create Entity 对话框、Hierarchy/File Explorer 右键菜单、Settings 四分区（含快捷键改绑、VSync、自动保存）、Project Settings 渲染质量；异步日志 AsyncLogger + 热路径性能优化）。
12. 之后进入 M3（渲染补完）：IBL、后处理栈、CSM、抗锯齿、材质预设库，
   渲染新特性直接在编辑器里做预览面板，边开发边验证；脚本系统（M4）的前置 Node 架构已就绪，可提前启动脚本语言选型。

---

## WPF Editor C API 路线图（当前进行中）

> ImGui 编辑器已完成，现迁移到 WPF 编辑器架构。WPF 编辑器通过 C API 桥接 Core DLL，
> 实现 Editor 与 Core 的完全分离。

### Phase 1 — 基础架构 + WPF Shell（已完成）

| 任务 | 状态 | 说明 |
|---|---|---|
| Core/Platform/Renderer/Physics 编译为 DLL | 已完成 | `libGryceCored.dll` 等 |
| `core/c_api.h` 统一 C API 入口 + 宏 | 已完成 | `GRYCE_C_API` 导出宏 |
| `GConfig_Init()` / `GConfig_Shutdown()` | 已完成 | Core  DLL 加载/卸载 |
| `GCommand` 命令队列（`GCommandType` + `GCore_PushCommand`） | 已完成 | 主线程安全命令队列 |
| `GEntity_*` 实体操作 API | 已完成 | Create/Destroy/GetName/SetName/GetParent/GetChild* |
| `GComponent_AddComponent` / `RemoveComponent` | 已完成 | 组件增删 |
| `GScene_New` / `Save` / `Load` | 已完成 | 场景管理 |
| `GCore_SetCallback_*` 回调注册（实体列表/选中/PlayMode/日志） | 已完成 | C# 侧 delegate 回调 |
| WPF Shell（MainWindow + Docking 布局面板） | 已完成 | 解决方案 `GryceEngine.sln`，项目名 `GryceEngine.Editor` |
| WPF P/Invoke 封装（Native/*.cs） | 已完成 | C# 侧所有 C API 的 P/Invoke 声明 |
| WPF 面板：Hierarchy / Inspector / Toolbar / Settings / Console / Project | 已完成 | 基础功能可用 |
| 构建通过 + 运行无 XamlParseException | 已完成 | 0 错误，WPF 正常启动 |

### Phase 2 — GryceRenderer + GrycePlatform C API（⬜ 待做）

| 任务 | 状态 | 说明 |
|---|---|---|
| `GWindow_InitExternal(hwnd)` — 接收外部 HWND，不创建 GLFW | ⬜ 待做 | WPF 提供 HWND，Renderer 嵌入 |
| `GRender_Init` — `init_with_hwnd` + `sync_mode=true`（禁用内部 RenderThread） | ⬜ 待做 | 同步模式，WPF 主线程驱动渲染 |
| `GRender_BeginFrame` / `RenderWorld` / `RenderGizmo` / `EndFrame` | ⬜ 待做 | 逐帧渲染 API |
| Viewport texture 输出：`GRender_GetViewportTexture` | ⬜ 待做 | WPF 获取渲染结果纹理 |
| Standalone test：HWND → 清屏色 + 空场景 | ⬜ 待做 | 验证渲染管线嵌入外部 HWND |

### Phase 3 — Component 反射桥接 + WPF Editor 接入（⬜ 待做）

| 任务 | 状态 | 说明 |
|---|---|---|
| `ECMD_ADD_COMPONENT` / `ECMD_REMOVE_COMPONENT` / `ECMD_SET_PROPERTY` | ⬜ 待做 | 命令类型扩展 |
| `GComponent_GetProperty` / `SetProperty`（通过 reflection 桥接） | ⬜ 待做 | Inspector 属性读写 |
| `GComponent_GetPropertyCount` / `GetPropertyInfo`（Inspector 自动发现字段） | ⬜ 待做 | 反射自动生成 Inspector |
| `GComponent_GetRegisteredTypeCount` / `GetRegisteredTypeInfo`（Add Component 下拉菜单） | ⬜ 待做 | 组件类型列表 |
| WPF Editor P/Invoke 封装（C# 侧 GryceCoreAPI / GryceRendererAPI） | ⬜ 待做 | 封装新增 API |
| WPF SwapChainPanel / WindowsFormsHost 提供 HWND → Renderer | ⬜ 待做 | Viewport 嵌入 |
| 闭环验证：加载场景 → Hierarchy → 选中 → Inspector 改属性 → Viewport 更新 | ⬜ 待做 | 端到端验证 |

### Phase 4 — PlayMode + 物理 + Gizmo（⬜ 待做）

| 任务 | 状态 | 说明 |
|---|---|---|
| `ECMD_PLAY_MODE` / `ECMD_STOP_MODE` + Scene 快照/回滚 | ⬜ 待做 | Play Mode 命令 |
| GrycePhysics.dll C API：`GPhysics_Init` / `Step` / `Raycast` / `CreateBody` | ⬜ 待做 | 物理 C API |
| GrycePlatform.dll 输入注入：`GInput_InjectKey` / `MouseMove` / `MouseButton` | ⬜ 待做 | WPF 输入转发到 Core |
| Core 内部 Viewport Toolbar + ImGuizmo | ⬜ 待做 | Gizmo 操作 |
| `ECMD_GIZMO_MANIPULATE` 写回 Transform | ⬜ 待做 | Gizmo 拖拽写回 |

### Phase 5 — 资产 + 完整闭环（⬜ 待做）

| 任务 | 状态 | 说明 |
|---|---|---|
| GryceCore/asset_api.h + `ECMD_IMPORT_ASSET` | ⬜ 待做 | 资产导入命令 |
| 日志转发：`GCore_GetLogMessages`（Console 窗口） | ⬜ 待做 | Console 面板日志 |
| 完整 Editor ↔ Core 闭环验证 | ⬜ 待做 | 全功能端到端测试 |
