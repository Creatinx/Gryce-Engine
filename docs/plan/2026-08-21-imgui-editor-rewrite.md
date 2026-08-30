# ImGui Editor 重写实现计划

> **For agentic workers:** 本计划按递进依赖关系组织，每个阶段依赖前一个阶段完成。

**Goal:** 用 C++/ImGui Docking 替代旧 WPF C# 编辑器，实现跨平台高性能编辑器

**Architecture:** 基于 Godot 4.7.2 编辑器功能分析，按递进依赖顺序分 10 个阶段实现。每个阶段在上一阶段基础上扩展，形成可独立测试的增量。

**Tech Stack:** C++23, ImGui Docking, GLFW, OpenGL/Vulkan, Gryce Engine Core

---

### Phase 0: 基础架构（已完成）

**基础框架核心文件：**
- `editor/CMakeLists.txt` — GryceEditor 目标定义
- `editor/src/main.cpp` — 入口点
- `editor/src/editor.h` — EditorApp 类接口
- `editor/src/editor.cpp` — 编辑器实现（含 ImGui Docking 布局）

**已完成功能：**
- Window 创建与渲染上下文初始化
- ImGui Docking 布局（Viewport/Hierarchy/Inspector/Console 面板）
- 主菜单栏（File/Edit/View/Help）
- 渲染管线集成
- 构建系统适配（CMake + build.py）

---

### Phase 1: 视口渲染（Viewport Rendering）

**依赖：** Phase 0

**目标：** 将 3D/2D 场景渲染到 Viewport 面板的 ImGui 纹理中

**文件：**
- 修改：`editor/src/editor.cpp` — render_viewport()
- 修改：`editor/src/editor.h` — 新增成员

**任务：**

1. **创建渲染目标纹理**
   - 在 `editor.h` 中添加 `render::TextureHandle viewport_tex_`
   - 在 `init()` 中创建 FBO 和纹理，尺寸跟随 Viewport 面板大小

2. **实现 Viewport 面板渲染**
   - 重写 `render_viewport()` 使用 `ImGui::Image()` 显示渲染纹理
   - 从 `render_ctx_` 获取渲染结果并绑定到 ImGui 纹理
   - 支持 Viewport 面板尺寸变化时自动调整渲染目标尺寸

3. **添加简单相机控制**
   - 3D 场景：实现鼠标拖拽旋转（Arcball/Orbit 相机）
   - 2D 场景：实现鼠标滚轮缩放 + 中键平移
   - 使用 `GLFW_KEY_*` + `glfwGetMouseButton()` 检测输入

4. **添加场景网格/Gizmo**
   - 在 Viewport 中绘制 3D 网格参考线
   - 添加坐标轴指示器（类似 Godot 左上角的 XYZ 轴）

---

### Phase 2: 层级面板（Hierarchy Panel）

**依赖：** Phase 1

**目标：** 显示场景中所有实体的树形结构，支持选择、创建、删除

**文件：**
- 修改：`editor/src/editor.cpp` — render_hierarchy()
- 新增：`editor/src/hierarchy_panel.h`
- 新增：`editor/src/hierarchy_panel.cpp`

**任务：**

1. **创建 HierarchyPanel 类**
   - 将 `render_hierarchy()` 逻辑抽取到独立类
   - 维护 `selected_entity_` 指针
   - 维护 `queued_deletes_` 列表（延迟删除，避免 GPU 竞争）

2. **实现实体树显示**
   - 递归遍历场景中所有实体（ParentComponent / ChildrenComponent）
   - 每个实体一行，缩进表示层级关系
   - 使用 `ImGui::Selectable()` 实现点击选择
   - 高亮选中实体

3. **实现实体创建/删除**
   - 右键菜单：“Create Entity” → 创建空实体
   - 右键菜单：“Delete” → 将实体加入 `queued_deletes_`
   - 选择实体时同步到 Viewport 高亮

4. **实现拖拽重排序**
   - 使用 ImGui 拖拽 API 实现实体父子关系重排
   - 拖拽到另一个实体上时设置为子实体

---

### Phase 3: 属性检查器（Inspector Panel）

**依赖：** Phase 2

**目标：** 显示选中实体的所有组件及其可编辑属性

**文件：**
- 修改：`editor/src/editor.cpp` — render_inspector()
- 新增：`editor/src/inspector_panel.h`
- 新增：`editor/src/inspector_panel.cpp`

**任务：**

1. **创建 InspectorPanel 类**
   - 构造时接收 `HierarchyPanel*` 获取选中实体
   - 枚举实体上的所有组件

2. **实现 Transform 组件编辑**
   - 显示 Position (Vec3)、Rotation (Vec3 欧拉角)、Scale (Vec3)
   - 使用 `ImGui::DragFloat3()` 实现拖拽编辑
   - 数值变化时通过反射系统更新组件

3. **实现通用组件属性编辑**
   - 遍历组件反射信息，为每个字段生成对应 ImGui 控件
   - 类型映射：float → DragFloat, int → DragInt, bool → Checkbox, string → InputText
   - Vector2/3/4 → DragFloat2/3/4, Color → ColorEdit4
   - 枚举字段 → Combo 下拉框

4. **实现组件添加/删除**
   - 底部“Add Component”按钮 → 弹出可选组件列表
   - 每个组件标题栏右侧“齿轮”菜单 → 删除 / Reset
   - 支持 CollapsingHeader 折叠/展开组件

5. **实现只读字段**
   - 根据 ProjectMemory 中的约定：Camera 组件的 FOV/Near/Far 为只读
   - 只读字段使用 `ImGui::Text()` 或 `BeginDisabled(true)` 包裹

---

### Phase 4: 文件资源浏览器（File Explorer Panel）

**依赖：** Phase 1

**目标：** 浏览项目资源目录，预览/导入资源文件

**文件：**
- 修改：`editor/src/editor.cpp` — render_file_explorer()
- 新增：`editor/src/file_explorer_panel.h`
- 新增：`editor/src/file_explorer_panel.cpp`

**任务：**

1. **创建 FileExplorerPanel 类**
   - 基于原有 `FileExplorerPanel` 逻辑（ProjectMemory 中的水平网格布局）
   - 扫描项目 `res/` 目录，按文件类型分类显示
   - 文件夹灰色文件夹图标，文件白色文档图标 + 彩色类型标识

2. **实现文件导航**
   - 双击文件夹进入子目录
   - 面包屑导航栏（路径回溯）
   - 上级目录“..”按钮

3. **实现资源预览**
   - 图片文件：显示缩略图
   - 音频文件：显示扬声器图标 + 橙色
   - 模型文件：显示 3D 图标
   - 文本文件：显示文件内容预览

4. **实现拖拽导入**
   - 拖拽资源文件到 Viewport → 创建对应实体
   - 拖拽图片到 Inspector 的贴图字段 → 赋值

---

### Phase 5: 场景管理（Scene Management）

**依赖：** Phase 2

**目标：** 多场景编辑、保存/加载、场景选项卡

**文件：**
- 修改：`editor/src/editor.cpp` — render_scene_tab_bar()
- 新增：`editor/src/scene_manager.h`
- 新增：`editor/src/scene_manager.cpp`

**任务：**

1. **创建 SceneManager 类**
   - 维护 `std::vector<std::unique_ptr<scene::Scene>> open_scenes_`
   - 当前激活场景索引 `active_scene_index_`

2. **实现场景选项卡**
   - 类似 Godot 顶部的 Scene 标签
   - 使用 `ImGui::TabBar()` + `ImGui::TabItem()`
   - 切换选项卡时更新 Hierarchy 和 Viewport 显示

3. **实现场景序列化**
   - 保存：`scene->save(path)` 序列化为 `.tscn` 格式
   - 加载：`scene->load(path)` 反序列化
   - 文件对话框使用 `ImGuiFileDialog` 或原生 OS 文件对话框

4. **实现场景操作**
   - 新建场景（Ctrl+N）
   - 打开场景（Ctrl+O）
   - 保存场景（Ctrl+S）
   - 另存为（Ctrl+Shift+S）
   - 关闭场景前检查是否有未保存更改

---

### Phase 6: 运行模式（Play Mode）

**依赖：** Phase 5

**目标：** 在编辑器中启动/停止游戏运行，查看实时效果

**文件：**
- 修改：`editor/src/editor.cpp` — render_toolbar()
- 新增：`editor/src/play_mode_manager.h`
- 新增：`editor/src/play_mode_manager.cpp`

**任务：**

1. **创建 PlayModeManager 类**
   - 管理编辑器模式：Edit ↔ Play
   - 进入 Play 时克隆当前场景作为运行实例
   - 退出 Play 时丢弃运行实例，恢复编辑场景

2. **实现工具栏按钮**
   - 播放按钮（▶）— 进入 Play Mode
   - 暂停按钮（⏸）— 暂停/恢复游戏逻辑
   - 停止按钮（⏹）— 退出 Play Mode
   - 使用 `ImGui::Button()` + 自定义图标

3. **实现游戏循环集成**
   - Play Mode 下运行 Engine 主循环（ECS System 更新）
   - 暂停时只更新 `pause_mode=true` 的脚本
   - 使用 `ScriptComponent::process_priority` 排序更新

4. **实现运行时调试**
   - Play Mode 下 Viewport 显示游戏实时渲染
   - Console 显示游戏运行时日志
   - 实体属性在 Play Mode 下只读

---

### Phase 7: 控制台面板（Console Panel）

**依赖：** Phase 0

**目标：** 显示引擎日志、错误、警告，支持过滤和搜索

**文件：**
- 修改：`editor/src/editor.cpp` — render_console()
- 新增：`editor/src/console_panel.h`
- 新增：`editor/src/console_panel.cpp`

**任务：**

1. **创建 ConsolePanel 类**
   - 维护 `std::vector<LogEntry> entries_` 环形缓冲区
   - LogEntry 结构：timestamp, level (Info/Warn/Error), message, source

2. **实现日志级别过滤**
   - 顶部按钮栏：Info / Warning / Error 切换过滤
   - 每个级别使用不同颜色（Info=白, Warn=黄, Error=红）
   - 使用 `ImGui::ColorButton()` 或文本颜色

3. **实现日志搜索**
   - 搜索框：`ImGui::InputText()` 实时过滤
   - 匹配行高亮
   - 支持正则搜索（可选）

4. **实现日志输出钩子**
   - 将 `GLog` 输出重定向到 ConsolePanel
   - 支持右键菜单复制日志文本
   - 支持清空日志

---

### Phase 8: 撤销/重做系统（Undo/Redo）

**依赖：** Phase 3

**目标：** 支持多步撤销/重做操作，Ctrl+Z/Y 快捷键

**文件：**
- 修改：`editor/src/editor.cpp` — 集成 CommandStack
- 新增：`editor/src/command_stack.h`
- 新增：`editor/src/command_stack.cpp`

**任务：**

1. **创建 CommandStack 类**
   - 基于原有 `CommandStack` 逻辑（ProjectMemory 中记录）
   - `push_command(std::unique_ptr<Command>)` 记录操作
   - `undo()` / `redo()` 回退/重做
   - 最大历史步数限制（默认 100 步）

2. **创建 Command 基类及派生类**
   - `Command` 基类：`virtual void execute() = 0`, `virtual void undo() = 0`
   - `MoveEntityCommand` — 移动实体位置
   - `DeleteEntityCommand` — 删除实体（含恢复）
   - `AddComponentCommand` — 添加组件
   - `ModifyPropertyCommand` — 修改属性值（存储旧值/新值）

3. **实现快捷键**
   - Ctrl+Z → Undo
   - Ctrl+Y → Redo
   - 使用 ImGui 快捷键 API 或 `ShortcutManager`
   - 确保文本输入框聚焦时不触发（`io.WantTextInput` 检查）

4. **集成到面板**
   - Hierarchy 面板的删除操作生成 `DeleteEntityCommand`
   - Inspector 面板的数值修改生成 `ModifyPropertyCommand`
   - 菜单栏 Edit → Undo/Redo 显示当前可撤销/重做操作名

---

### Phase 9: 动画编辑器（Animation Editor）

**依赖：** Phase 5

**目标：** 编辑关键帧动画，支持时间线和播放控制

**文件：**
- 新增：`editor/src/animation_editor_panel.h`
- 新增：`editor/src/animation_editor_panel.cpp`
- 修改：`editor/src/editor.cpp` — 注册新面板

**任务：**

1. **创建 AnimationEditorPanel 类**
   - 时间线视图：`ImGui::BeginChild()` + 自定义绘制
   - 播放头：可拖拽指示当前帧位置
   - 帧刻度：显示帧号（0, 10, 20, ...）

2. **实现关键帧管理**
   - 选中实体时显示其 AnimationComponent 的关键帧
   - 右键轨道 → 添加关键帧
   - 拖拽关键帧调整时间位置
   - 删除关键帧（右键菜单或 Del 键）

3. **实现动画播放控制**
   - 播放/暂停/停止按钮
   - 循环播放开关
   - 播放速度控制（0.5x, 1x, 2x）
   - 当前帧/总帧数显示

4. **实现属性轨道**
   - 每个可动画属性一条轨道
   - 支持：Transform (position/rotation/scale)、颜色、数值
   - 轨道折叠/展开

---

### Phase 10: 着色器编辑器（Shader Editor）

**依赖：** Phase 5

**目标：** 编辑和预览着色器代码

**文件：**
- 新增：`editor/src/shader_editor_panel.h`
- 新增：`editor/src/shader_editor_panel.cpp`
- 修改：`editor/src/editor.cpp` — 注册新面板

**任务：**

1. **创建 ShaderEditorPanel 类**
   - 代码编辑器区域：使用 `ImGui::InputTextMultiline()` 或集成 TextEditor
   - 语法高亮：自定义着色器关键字高亮
   - 行号显示

2. **实现实时编译与预览**
   - 编译按钮 + 自动编译（可选延迟 500ms）
   - 编译错误显示在面板底部
   - 预览 Viewport 实时显示着色器效果

3. **实现着色器参数编辑**
   - 解析 uniform 定义并生成对应控件
   - float → Slider, vec3 → ColorEdit3, sampler2D → 纹理选择

4. **实现着色器模板**
   - 新建时提供模板选择：Unlit、PBR、Post-process
   - 保存/加载着色器文件

---

### Phase 11: 项目设置与导出（Project Settings & Export）

**依赖：** Phase 5

**目标：** 编辑项目设置、配置导出参数

**文件：**
- 新增：`editor/src/project_settings_dialog.h`
- 新增：`editor/src/project_settings_dialog.cpp`
- 修改：`editor/src/editor.cpp` — 注册菜单项

**任务：**

1. **创建项目设置对话框**
   - 菜单栏 Edit → Project Settings
   - 模态对话框：`ImGui::BeginPopupModal()`
   - 分类设置：General、Rendering、Audio、Input

2. **实现设置编辑**
   - 窗口标题、分辨率、默认场景
   - 渲染 API（OpenGL/Vulkan）
   - 音频设置
   - 输入映射（Key → Action）

3. **实现导出配置**
   - 导出目标平台选择（Windows/Linux）
   - 资源打包选项
   - 图标设置
   - 导出路径

4. **实现设置持久化**
   - 保存到 `project.gryce` 配置文件
   - 加载时恢复设置
   - 设置变更时提示保存

---

### Phase 12: 主题与打磨（Theme & Polish）

**依赖：** Phase 0–11

**目标：** 美化编辑器界面，支持主题切换

**文件：**
- 修改：`editor/src/editor.cpp` — 主题应用
- 新增：`editor/src/theme_manager.h`
- 新增：`editor/src/theme_manager.cpp`

**任务：**

1. **创建 ThemeManager 类**
   - 管理暗色/亮色主题
   - 自定义颜色方案（基于现有 engine_style）
   - 主题应用到 ImGui::GetStyle()

2. **实现主题切换**
   - 菜单栏 View → Theme → Dark / Light / Modern Light
   - 切换时立即刷新所有面板
   - 保存用户偏好设置

3. **实现滚动条美化**
   - 16px 宽度，高对比度拖拽块
   - 圆角滚动条（与整体风格一致）

4. **实现字体优化**
   - 支持自定义字体大小
   - 中文字体支持
   - 字体抗锯齿

5. **实现 DPI 适配**
   - 检测系统 DPI 缩放
   - 自动调整 ImGui 缩放比例
   - 高 DPI 下字体/图标清晰显示

---

### 执行顺序总结

```
Phase 0: 基础架构 ────────────────────────（已完成）
Phase 1: 视口渲染 ────────────────────────（依赖 Phase 0）
Phase 2: 层级面板 ────────────────────────（依赖 Phase 1）
Phase 3: 属性检查器 ──────────────────────（依赖 Phase 2）
Phase 4: 文件资源浏览器 ──────────────────（依赖 Phase 1）
Phase 5: 场景管理 ────────────────────────（依赖 Phase 2）
Phase 6: 运行模式 ────────────────────────（依赖 Phase 5）
Phase 7: 控制台面板 ──────────────────────（依赖 Phase 0）
Phase 8: 撤销/重做系统 ──────────────────（依赖 Phase 3）
Phase 9: 动画编辑器 ──────────────────────（依赖 Phase 5）
Phase 10: 着色器编辑器 ──────────────────（依赖 Phase 5）
Phase 11: 项目设置与导出 ────────────────（依赖 Phase 5）
Phase 12: 主题与打磨 ────────────────────（依赖 Phase 0–11）
```

总代码量预估：~15,000–20,000 行 C++（不含 ImGui 和引擎核心）