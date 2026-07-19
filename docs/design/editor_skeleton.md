# 编辑器骨架设计说明（M1-E1 后半）

> 范围：ImGui Docking 布局 + 面板框架、编辑器自由飞行相机、GLOG 内存捕获、
> 渲染到纹理的 Viewport 嵌入。代码位于 `editor/`，构建目标 `gryce`（gryce.exe）。

## 总体结构

```
editor/
  main.cpp               入口，仅调用 EditorApp::run()
  editor_app.{h,cpp}     编辑器主循环（原 main.cpp 的 demo 逻辑 + 编辑器集成）
  editor_panel.h         EditorPanel 基类（名称 / 可见性 / Begin-End 包装）
  panel_manager.{h,cpp}  DockSpace 宿主 + DockBuilder 默认布局 + Window 菜单
  editor_camera.{h,cpp}  自由飞行相机（ImGui IO 驱动，不依赖场景相机）
  panels/
    hierarchy_panel      场景实体树（只读 + 选中）
    inspector_panel      选中实体组件字段编辑（走 core/reflection）
    console_panel        GLOG MemoryLogSink 显示（级别过滤 / 自动滚动 / 清空）
    viewport_panel       渲染到纹理嵌入（ImGui::Image）
    project_panel        占位（显示项目根目录）
  project/               编辑器自带项目（project.gryce + shaders/models/textures/...）
```

## 关键决策

### 1. 线程模型（视口纹理）

沿用既有 ImGui 渲染模型：主线程 `begin_frame()` → 面板 `show()` 记录绘制命令 →
`end_frame(callback)` 中 `clone_draw_data` 后 `push_command` 到渲染线程执行。
OpenGL 端 ImTextureID 即 GLuint，纹理对象 id 创建后不可变，主线程采样记录、
渲染线程绑定，无需额外同步。`IImGuiBackend::imgui_texture_id(ITexture*)` 做后端
分派：GL 返回 `GLTexture::texture_id()`；VK 返回 0（descriptor 注册未实现，
Viewport 面板显示占位文本）。

### 2. 渲染到纹理

`RenderPipeline` 新增视口离屏输出：`set_viewport_output_enabled(true)`（init 前）
创建 RGBA8 颜色纹理 + 独立 FBO；开启时 `render_tonemap()` 写入该 FBO 而非默认
framebuffer（tonemap 后的 LDR 恰好是 ImGui 需要的显示格式，HDR 中间纹理不动）。
面板尺寸变化时 `resize_render_targets()` 重建 HDR + 视口目标——**先创建新目标
再销毁旧目标**，避免句柄槽位复用导致悬垂引用；调用前必须
`pause_render_thread()`（GL 对象在主线程重建），EditorApp 侧加 0.15s 防抖，
避免拖动 dock 分隔条时反复 pause/resume。

Vulkan 端本轮不支持：shader 与 FBO 的 render pass 绑定，重建需要额外处理；
VK 下管线仍直渲屏幕，Viewport 面板显示占位。

### 3. Docking 布局

PanelManager 建全屏宿主窗口（NoTitleBar/NoResize + PassthruCentralNode），
首跑（ini 中无 DockSpace 节点记录）用 DockBuilder 构建默认布局：
左 Hierarchy、右 Inspector、下 Console+Project、中 Viewport。此后布局完全交给
`imgui.ini` 持久化。`io.IniFilename` 指向 `editor/project/editor_imgui.ini`，
避免写进 build/ 或工作目录。Window 菜单可切换各面板可见性。

### 4. 编辑器相机

独立类 `EditorCamera` 包装 `math::Camera`（复用矩阵/向量计算），输入走 ImGui IO，
**只在 Viewport 面板悬停时响应**：右键按住拖动转视角、WASD+QE 平移、Shift 加速、
滚轮调速（0.1–100 clamp）、F 聚焦为占位（GLOG_DEBUG）。原 demo 的
Tab 锁定鼠标 / 自定义光标逻辑在编辑器中移除。

### 5. GLOG 内存 sink

`MemoryLogSink`（tee 模式）：包装 ConsoleLogger，日志照常输出 stderr，同时写入
容量 1000 的环形缓冲（独立 mutex，渲染线程写日志也安全）。Console 面板
`snapshot()` 拷贝读取 + 级别过滤。`MemoryLogSink::from_glog()` 供面板取回 sink。

### 6. 项目目录

各 example 以 `examples/<exe>/project.gryce` 为项目根。编辑器同理自带
`editor/project/`（资源从 3dtest 复制并补 skinned_pbr shader）。
`find_project_root()` 顺序：引擎根 → `editor/project/` → 回退 `examples/3dtest/`。

## 遗留项

- **Vulkan Viewport**：`IImGuiBackend::imgui_texture_id` VK 实现（descriptor 注册）
  + 管线 render pass 兼容的 FBO 重建。
- **F 聚焦**：接入选中实体包围盒计算相机目标位。
- **Hierarchy 选中指针**：实体运行时销毁可能悬垂（热重载已处理：换场景即清空）；
  后续应改为 EntityID/UUID 弱引用。
- **旧调试面板**：`ui/debug_panel.h`（DebugPanel/ModelLoaderPanel）暂未接入新框架，
  线框开关暂由 F1 保留；E2 时决定并入或移除。
- **Inspector**：Quaternionf 只读展示（待欧拉角 + gizmo）；无撤销/重做（E5）。
- 视口图像目前按面板尺寸 1:1 渲染，无 letterbox；Game View 的固定宽高比留待 E4。

## M1-E2 场景编辑闭环（追加）

### 选中：UUID 弱引用
HierarchyPanel 持有 `scene::UUID` 而非 `Entity*`，每次访问经
`Scene::find_entity_by_uuid` 解析；实体删除/场景热重载后解析失败自动清除，
根除悬垂指针。拖拽 payload 同样只携带 UUID 字符串。

### 结构变更延迟执行
树遍历（TreeNodeEx 递归）期间禁止改动场景结构：删除/换父记入
`PendingOp`，帧末统一执行。换父用 `detach_child + add_child`
（或从 `Scene::roots_` 摘出），含环检测（禁止拖到自身后代）。

### 点选拾取
`core/math/ray.h`：`screen_ndc_to_ray`（NDC z∈[-1,1] 两点反投影）+
`ray_intersect_aabb`（slab 法）。拾取遍历带 MeshRenderer 的实体，
mesh 顶点本地 AABB → 8 角点世界变换 → 世界 AABB → 求交取最近。
不依赖物理碰撞体，编辑器规模下线扫足够。

### Transform Gizmo
ImGuizmo 捆绑进 `third_party/imguizmo`（仅编辑器目标编译，`-w` 关警告）。
视口 `ImGui::Image` 上直接 `SetRect` 叠加绘制；操作世界矩阵后
`local = parent_world⁻¹ × world`，TRS 分解用自研
`Quaternionf::from_rotation_matrix`（Shepperd 法，与引擎 to_matrix 转置约定匹配）。
gizmo 激活（IsUsing/IsOver）时屏蔽编辑器相机与拾取。

### 既有约定注意
`Matrix4f Quaternionf::to_matrix()` 相对标准列向量约定为转置布局
（M×v 等价 q⁻¹vq），渲染全链路自洽，属历史约定；新增的
`from_rotation_matrix` 已按此约定转置读取并有回归测试覆盖。

### 遗留
- Scene View 网格线（Grid）未做。
- enum 字段反射不支持，Inspector 跳过（需反射系统扩展 FieldType::Enum）。
- 拾取为 AABB 精度（非逐三角形）；大场景可加 BVH。
- gizmo 分解不处理负缩放（镜像）。
- 换父后物理 body 的世界变换同步依赖下一物理步，跨层级拖带刚体可能有瞬时插值异常（未实测）。
