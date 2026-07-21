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
| 编辑器摄像机（Scene View 自由飞行相机） | **已完成** | `editor/editor_camera.*`：右键视角 + WASD/QE + 滚轮调速 + Shift 加速，F 聚焦占位；仅 Viewport 悬停时响应 |
| GLOG 捕获到内存 buffer | **已完成** | `core/utils/glog/`：MemoryLogSink（tee 模式环形缓冲，容量 1000，线程安全），Console 面板级别过滤 + 自动滚动 |
| 渲染到纹理 + 视口嵌入 ImGui | **已完成（OpenGL）** | RenderPipeline 视口离屏输出（tonemap → 独立 FBO），Viewport 面板 ImGui::Image 嵌入；面板尺寸防抖同步渲染目标；Vulkan 端 descriptor 注册留待后续 |

### E2 场景编辑核心（MVP 闭环）

| 任务 | 状态 | Unity 对应 |
|---|---|---|
| Hierarchy 面板（Entity 树、增删、拖拽换父、Prefab 标记） | **已完成** | 右键菜单（创建/重命名/删除）、拖拽换父（含拖回根级、环检测）、UUID 弱引用选中、[P] Prefab 标记；删除/换父延迟到帧末执行防迭代器失效 |
| Inspector 面板（基于反射自动生成组件属性编辑） | **已完成** | 反射字段自动分派控件（Drag/Slider/Checkbox/InputText），只读灰显，enabled 勾选；enum 未支持（反射限制，跳过） |
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
| HDR/EXR 环境贴图加载 | 待实现 | M3 | `.hdr` / `.exr` 已接入 AssetManager，需做 IBL |
| IBL（Image-Based Lighting） | 待实现 | M3 | irradiance + prefilter + BRDF LUT |
| Cubemap Mipmap 生成 | 待实现 | M3 | 当前 cubemap 强制 1 mip |
| 材质预设库（MaterialLibrary） | 待实现 | M3 | 内置 metal/plastic/glass/wood，编辑器一键应用 |
| 后处理栈（Bloom 补全 / Color Grading LUT / Volume 配置） | 待实现 | M3 | Vulkan 2D bloom shader 缺失；Unity Post-Processing Volume |
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
| GPU-Driven Rendering / Indirect Draw | 远期目标 | 海量物体 |
| D3D12 / Metal 后端 | 远期目标 | 多平台图形 API |
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
| 脚本语言选型与集成（Lua / C# / Python） | 待实现 | M4 | 推荐 Lua 或 C# |
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
| 用户手册 / 快速入门 | 待实现 | M6 | 面向开发者 |
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
11. 之后进入 M3（渲染补完）：IBL、后处理栈、CSM、抗锯齿、材质预设库，
   渲染新特性直接在编辑器里做预览面板，边开发边验证。
