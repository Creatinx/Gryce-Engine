# GryceIDE 完整化路线

本文是 GryceUI Phase 0–5 完成后的下一阶段计划。最终目标是仅使用 GryceUI
实现 IDE 的全部界面控件，并把 GryceEngine 的 3D/2D 场景作为面板内纹理嵌入。

## 0. 当前实施状态（2026-11-07）

- Phase 6 已完成：3D、Canvas2D 离屏纹理与 `UIViewportPanel` 已接通。
- Phase 7 已完成：焦点、快捷键、拖放、剪贴板、Dock、FloatingPanel、TabBar 和
  Splitter 已进入实际 IDE 路径。
- Phase 8 的通用控件主体已完成：数值输入、搜索、Vector、颜色、范围滑条、
  快捷键录制、菜单、树、表格、PropertyGrid、AssetGrid、对话框、Toast 和
  Console/CodeEditor 均有 `NodeUI` 实现与无头状态机测试。
- Phase 9 的壳层已接入：菜单栏、工具栏、Dock 布局、中央文档 Tab、状态栏、
  Command Palette、布局保存/恢复和 Ctrl+S/Ctrl+Shift+P 等命令已可用。
- Phase 10/11 已有最小编辑闭环：Hierarchy 使用真实父子树，支持创建、删除、
  复制、重命名、选择、Transform、撤销重做、场景保存；Assets 可打开 `.js`
  到内嵌编辑器，Console 和 Problems 已切换为真实控件。
- 中央文档区支持 3D Scene、2D Scene、3D + 2D 分屏、Game View 和 Script；
  OpenGL 与 Vulkan 的 IDE 截图已显示 Dock、工具栏、底部面板和内嵌视口。
- D3D12 渲染路径可运行；自动 PNG 回读文件已能生成，但当前仍全黑，需要继续
  定位 present/back-buffer 回读路径。

## 1. 最终验收标准

完成态必须满足：

- IDE 主窗口、菜单、工具栏、停靠区、标签页、场景树、Inspector、资源浏览器、
  Console、代码编辑器、对话框、弹窗和通知全部由 GryceUI 绘制。
- IDE 不依赖 ImGui 实现自身界面；ImGui 只保留为引擎调试工具和可选排障入口。
- Scene View 和 Game View 在 UI 面板内渲染，不再只能铺满整个窗口。
- 至少支持一个 3D Scene View、一个 2D Scene View、一个 Game View 和多个
  可切换相机视图；面板缩放不会拉伸或错位。
- 能打开项目、创建/删除/选择实体、修改组件属性、撤销重做、保存场景。
- 能浏览和导入资源、打开脚本、编辑并保存、查看 Console 和编译错误。
- 能进入 Play、暂停、单步、停止，并从 IDE 构建和运行项目。
- 关闭 IDE 后恢复布局、打开文档、选择状态和视口状态。

## 2. 架构边界

保持单一路径：

```text
NodeUI / UIRoot
     ↓
UIRuntime 布局、输入、焦点、事件
     ↓
UIDrawList
     ↓
engine/render/ui_pipeline
     ↓
IRenderer2D overlay
     ↓
OpenGL / Vulkan / D3D12
```

IDE 层只负责：

- 编辑器文档和选择状态
- 面板组织
- 命令与快捷键
- 项目/资源/构建流程
- 引擎视口管理

引擎层不包含 IDE 面板逻辑。GryceUI 不直接读取场景、资源或编辑器状态；IDE
adapter 把它们转换成 NodeUI 属性和命令。

## 3. 阶段 6：内嵌视口与纹理桥（已完成）

这是最高优先级。没有稳定的视口纹理，场景树和 Inspector 做得再完整也无法形成 IDE。

### 交付

- `GryceUI::TextureRef`：稳定的 64 位纹理引用，不暴露后端裸指针。
- 引擎视口纹理查询接口：把 `GRender_GetViewportTexture()` 等句柄转换为
  `TextureRef`，并带 generation 失效检查。
- `UIViewportPanel`：
  - 接收目标纹理、源尺寸和 UV
  - 保持宽高比
  - 处理面板缩放和 HiDPI
  - 转发鼠标坐标到视口坐标
  - 内置焦点、hover、拖拽和右键上下文
- `EditorViewportTarget`：
  - Scene3D / Scene2D / Game
  - 独立相机和尺寸
  - 离屏颜色输出
  - resize 合并，避免拖拽窗口时每帧重建
- 3D 场景离屏输出接入现有 `GRender_SetViewportOutput(true)`。
- Canvas2D 增加离屏目标；3D、2D 和 gizmo 使用同一视口尺寸。
- 多个视口共用场景数据，但各自保存相机、显示模式和可见层。

### 验收

- Scene View 显示在 UI 面板内部，面板外仍显示 IDE 背景。
- 2D Scene View 和 3D Scene View 可以同时存在。
- 拖动分隔条改变尺寸后，纹理、命中区域和 gizmo 同步更新。
- 窗口最小化、场景热重载和切换后端后无失效纹理。
- OpenGL / Vulkan / D3D12 截图均能显示内嵌视口。

当前实现：

- `GRenderTextureInfo` 与 `GRender_GetViewportTextureInfo` /
  `GRender_GetGameViewTextureInfo` / `GRender_GetCanvas2DViewTextureInfo`
  提供稳定 RHI generation + 尺寸。
- 3D viewport output 与 Canvas2D viewport output 使用独立开关。
- `UIViewportPanel` 负责 letterbox、宽高比和纹理绘制。
- `GryceIDE.exe` 默认把 3D 场景显示在面板内，`--view2d` 或顶部按钮可切换到
  Canvas2D 离屏纹理。
- OpenGL 已验证内嵌 3D 纹理区域包含 2,095 个采样颜色，不再是清屏色。

## 4. 阶段 7：控件与交互基础（已完成）

在进入 IDE 壳之前，先补齐通用交互能力。

### 基础层

- 主题 token 与最小样式表
- Popup / Modal / Tooltip 独立图层
- 鼠标捕获、键盘捕获、焦点环和 Tab 顺序
- 快捷键、命令路由和文本输入门控
- 剪贴板、IME、Unicode/CJK 字体 fallback
- 拖放协议
- 滚动条、滚动边界和滚轮惯性
- 双缓冲 DrawList、脏矩形和按需重建

### 布局与容器

- Anchor / Min / Max
- Horizontal / Vertical / Grid
- Splitter
- DockHost
- DockPanel
- DockTab
- FloatingPanel
- DropZone

### 验收

- 没有控件占全局输入时，快捷键回到 IDE 命令层。
- 弹层、模态和工具提示不会被普通面板裁掉。
- Dock 拖拽、拆分、合并和关闭都可逆。
- 文本输入不会被快捷键抢键。

当前已实现：Splitter、DockHost/DockPanel 五区域布局、TabBar、ComboBox、
ProgressBar、ListView、Popup/Modal/Tooltip、跨控件拖放、FloatingPanel，
以及 UI 快捷键命令队列和平台剪贴板桥。鼠标/键盘焦点、IME、Tab 导航和布局
最小尺寸在前序阶段已经完成。IDE 默认布局已经迁移到 DockHost。

## 5. 阶段 8：完整控件集（通用控件主体已完成）

控件按依赖顺序实现，不一次铺开。

### 8.1 输入与数值

- TextInput
- MultilineTextInput
- SearchBox
- NumberInput
- DragFloat / DragInt
- Vector2 / Vector3 / Vector4 Editor
- ColorPicker
- KeyBindingEditor

### 8.2 选择与菜单

- Button
- IconButton
- ToggleButton
- CheckBox
- RadioButton
- ComboBox / Select
- MenuBar
- Menu
- MenuItem
- ContextMenu
- Toolbar
- Breadcrumb

### 8.3 数据与浏览

- ListView
- TreeView
- TableView
- PropertyGrid
- AssetGrid
- TabBar / TabView
- ScrollView
- RangeSlider
- ProgressBar
- StatusBar

### 8.4 对话框与反馈

- Tooltip
- Popup
- Modal
- MessageDialog
- ConfirmDialog
- FileDialog
- FolderPicker
- Toast
- NotificationCenter

### 验收

每个控件必须具备：

- hover / pressed / focused / disabled
- 键盘操作
- 序列化状态中合理的持久字段
- 无头状态机测试
- 单后端截图对比
- 不依赖 IDE 类型

当前实现覆盖：

- `UISearchBox`、`UINumberInput`、`UIDragFloat`、`UIVectorEditor`、
  `UIColorPicker`、`UIRangeSlider`、`UIKeyBindingEditor`
- `UIRadioButton`、`UIToggleButton`、`UIIconButton`、`UIBreadcrumb`、
  `UIMenuBar`、`UIMenu`、`UIContextMenu`、`UIToolbar`
- `UITreeView`、`UITableView`、`UIPropertyGrid`、`UIAssetGrid`、
  `UIStatusBar`
- `UIMessageDialog`、`UIConfirmDialog`、`UIFileDialog`、`UIFolderPicker`、
  `UIToast`、`UINotificationCenter`
- `UICodeEditor`、`UIConsoleView`

下一步仍需补的通用能力是主题 token 的统一下发、跨控件拖放的自定义 payload
渲染、文件对话框的真实宿主适配，以及所有态的截图基线。

## 6. 阶段 9：IDE Shell（核心壳层已实现）

### 交付

- 主菜单和快捷命令
- 工具栏与可配置按钮
- 可停靠面板系统
- 标签页文档区
- 底部状态栏
- Command Palette
- 布局保存/恢复/重置
- 最近项目
- 主题切换
- 崩溃恢复自动保存

### 默认布局

```text
┌ Menu / Toolbar ──────────────────────────────┐
│ Hierarchy        Scene View        Inspector │
│                                               │
│                  Game View                    │
├ Assets / Project ─ Console / Problems ───────┤
└ Status Bar ──────────────────────────────────┘
```

### 验收

- 用户可以拖动面板、拆分区域、合并 tab、恢复默认布局。
- 重启 IDE 后恢复布局。
- 不同 DPI 和窗口尺寸下布局不重叠。

当前已实现菜单栏、工具栏、默认五区域 Dock、中央 3D/2D/Script 文档 Tab、
底部 Assets/Console/Problems Tab、状态栏、Command Palette 和
`.gryce/ide_layout.json` 布局恢复。最近项目、主题切换和崩溃恢复仍待补。

## 7. 阶段 10：项目、场景与资源编辑（最小闭环已实现）

### Project

- 打开/关闭/创建项目
- `project.data` 设置
- 最近项目
- 资源导入和导入选项
- 文件变更监听

### Scene

- Entity 创建、删除、复制、粘贴、重命名
- Hierarchy 树
- Reparent、排序和激活状态
- 反射驱动 Inspector
- Transform 多选编辑
- Gizmo 平移/旋转/缩放
- 撤销/重做
- Scene 保存与热重载

### Assets

- 目录树和网格/列表视图
- 图标、缩略图、筛选和搜索
- 拖入场景/Inspector
- 资源引用与 miss 状态
- 双击打开文本、材质、场景和模型预览

### 验收

- 从 Hierarchy 选择实体后 Inspector 与 Gizmo 同步。
- 修改 Transform 走命令和撤销栈。
- 资源拖放能创建组件或设置属性。
- 保存后重新打开场景得到等价内容。

当前可创建、删除、复制、重命名和选择实体，编辑局部 Transform，撤销/重做与
场景保存；Hierarchy 已从扁平按钮改为真实父子树。Inspector 已通过反射表生成
可编辑属性行并提交 `GEdit_SetProperty`；材质嵌套字段、资源引用和多选编辑待接。

## 8. 阶段 11：代码、调试与构建（最小编辑闭环已实现）

### Code Editor

- 多标签文本文件
- 行号和当前行
- UTF-8
- 选择、复制、粘贴、撤销/重做
- 查找/替换
- 大文件虚拟行渲染
- JS 语法高亮
- 保存与热重载状态

### Console

- 日志等级、过滤和搜索
- 自动滚动与暂停
- 双击定位源码
- 清空和导出

### Run / Build

- Play / Pause / Step / Stop
- Game View 和编辑器 Scene View 独立
- 构建、运行、打包
- 错误列表跳到文件/行
- 进程输出流接入 Console

### 验收

- 打开脚本、修改、保存，热重载可见结果。
- Play 后场景运行，进入编辑态后恢复快照。
- 构建错误点击后打开对应文件位置。
- 构建/运行/打包均可在 IDE 内完成。

当前代码编辑器具备 UTF-8 多行输入、行号、当前行、文件打开/保存和脏状态；
Console/Problems 是 GryceUI 控件。构建进程输出、错误跳转、断点和独立 Game
View 尚未接入。

## 9. 阶段 12：稳定性、性能与发布

- 多视口纹理生命周期和 generation 校验
- Dock 大量面板时 DrawCall 和内存预算
- 字体：Latin + CJK fallback，动态图集
- 多窗口、多 DPI、窗口最小化和设备失去
- 大场景 Hierarchy 虚拟列表
- Inspector 属性编辑的批量刷新
- 崩溃恢复、日志持久化
- 自动测试：
  - 控件状态机
  - Dock 布局
  - 视口坐标和纹理失效
  - 场景编辑/撤销
  - 资源拖放
  - 代码编辑和构建
  - 三后端截图
- Windows 安装包和便携版

## 10. 推荐执行顺序

```text
6  视口与纹理桥
7  控件交互基础 + Dock/Splitter
8  完整控件集
9  IDE Shell
10 项目/场景/资源编辑
11 代码/Console/Build
12 性能与发布
```

其中阶段 6 和 7 必须先完成。没有稳定视口和焦点/弹层/停靠能力，阶段 8 的
控件会因宿主行为变化反复返工。

## 11. 明确不做

- 不把 IDE 面板逻辑塞进 GryceEngine.dll。
- 不为每个控件创建一个 ECS Entity。
- 不为 UI 复制 OpenGL/Vulkan/D3D12 图形后端。
- 不在第一个可用版本前实现插件市场、主题商店或完整动画时间轴。
- 不维护 ImGui 和 GryceUI 两套 IDE 控件实现。
