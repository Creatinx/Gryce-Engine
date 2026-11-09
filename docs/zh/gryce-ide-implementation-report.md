# GryceIDE / GryceUI 实施与架构调查报告

日期：2026-11-07

## 1. 结论

当前已经从旧 UiKit / ImGui 宿主路线切换到单一 GryceUI 路径：

```text
NodeUI / UIRoot
    ↓
UIRuntime（布局、命中、焦点、滚动、拖放、命令）
    ↓
UIDrawList
    ↓
render_ui_draw_list
    ↓
IRenderer2D overlay
    ↓
OpenGL / Vulkan / D3D12
```

这解决了原项目中的主要结构问题：

- 不再维护四套 UI 后端。
- 不再为每个控件创建 ECS Entity。
- UI 树和 Entity 树职责分开。
- 3D、Canvas2D、UI 使用独立输出和统一 RHI 提交。
- IDE 宿主只持有编辑状态和命令，不把面板逻辑塞回引擎。

## 2. 节点与渲染边界

节点继承保持：

```text
Component
└── Node
    ├── Node3D
    └── Node2D
        └── NodeUI
```

`Component2D` 表示可绘制 2D 特性，不作为节点基类。UI 控件只实现：

- 属性和序列化。
- `on_ui_event` 状态机。
- `add_draw_commands` 输出后端无关命令。

GPU 资源、纹理 generation、手柄和 scissor 都由渲染层处理。

## 3. 渲染管线修复

### 3.1 OpenGL 裁剪

UI 裁剪矩形使用左上原点，而 `glScissor` 使用左下原点。此前没有转换，
表现为面板背景可见、面板内子控件消失。现在由 OpenGL 后端集中转换 Y 坐标，
Renderer2D 和 UI 控件保持统一左上坐标系。

### 3.2 Vulkan UI overlay

`VulkanRenderer2D` 之前没有实现 `begin_overlay_pass` / `end_overlay_pass`。
UI DrawList 虽然生成，但没有切换到左上原点、屏幕空间的覆盖层状态，因此
Vulkan 截图只剩清屏色或局部纹理。

现在 Vulkan 覆盖层会：

- 先提交场景 2D 批次。
- 切换到交换链默认目标。
- 使用 1:1、左上原点的正交投影。
- 关闭 bloom、深度和剔除。
- 提交 UI 批次后恢复相机与目标。

### 3.3 多视口

IDE 中央文档区当前支持：

- 3D Scene View。
- 2D Scene View。
- 3D + 2D 分屏。
- Game View。
- Script。

3D 与 Canvas2D 的 viewport output 使用独立开关。分屏时两张纹理同时存在，
两个 `UIViewportPanel` 分别消费各自的 RHI 纹理。

## 4. GryceUI 控件

新增控件集中在：

- `engine/ui/ui_extended_widgets.h`
- `engine/ui/ui_editor_widgets.h`

已实现：

- 输入：TextInput、MultilineTextInput、SearchBox、NumberInput、DragFloat、
  VectorEditor、ColorPicker、RangeSlider、KeyBindingEditor。
- 选择与菜单：RadioButton、ToggleButton、IconButton、Breadcrumb、
  MenuBar、Menu、ContextMenu、Toolbar。
- 数据：ListView、TreeView、TableView、PropertyGrid、AssetGrid、
  StatusBar、ConsoleView。
- 反馈：MessageDialog、ConfirmDialog、FileDialog、FolderPicker、Toast、
  NotificationCenter。
- 文档：CodeEditor。

弹层的 `visible` 已纳入 `is_visible_in_tree()`，隐藏的菜单、模态框和 Toast
不会再拦截鼠标。

## 5. GryceIDE 最小闭环

当前 `GryceIDE.exe` 可以：

- 用 DockHost 组织 Hierarchy、Scene、Inspector、Assets/Console/Problems。
- 在 Hierarchy 中显示真实 Entity 父子树。
- 创建、删除、复制、重命名和选择实体。
- 编辑局部 Transform。
- Undo / Redo、Play / Pause / Step / Stop、Reload。
- 保存场景和布局。
- 打开项目中的 `.js` 文件到内嵌代码编辑器。
- 使用 Ctrl+S 保存活动文档、Ctrl+Shift+P 打开 Command Palette。
- 恢复 `.gryce/ide_layout.json`。

## 6. 验证

- CMake 全量构建成功。
- `GryceUiSkeletonTests` 覆盖布局、输入、DrawList、运行时和 9 组控件测试。
- 全量 CTest 41 条通过。并行运行时 `component_capi` 曾出现一次退出等待超时，
  单测和后续复跑均通过。
- OpenGL 截图验证：工具栏、Dock、Hierarchy、Assets、代码编辑器和内嵌 3D。
- Vulkan 截图验证：UI overlay、工具栏、面板与内嵌 3D。
- 3D + 2D 分屏和 Game View 均完成实际截图。

临时截图位置：

```text
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_release_opengl.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_release_vulkan.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_final_3d.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_final_split.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_final_game.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_vulkan_overlay.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_current_assets.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_current_script.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_current_palette.png
C:/Users/Creatinx/AppData/Local/Temp/gryce_ide_reflection.png
```

## 7. 已知问题

- D3D12 能运行 IDE 和生成 PNG，但当前回读结果为全黑，需继续定位 present target
  与 back buffer 的截图状态。
- Inspector 已有反射行生成和 `GEdit_SetProperty` 提交路径；材质嵌套对象、资源引用
  和多选编辑仍需专用编辑器。
- FileDialog/FolderPicker 已有控件状态机，真实文件系统适配仍在 IDE 层。
- 资源拖入 Inspector、引用 miss 状态和缩略图缓存尚未接入。
- CodeEditor 还缺查找/替换、虚拟行和 JS 语法高亮。
- Console 尚未接进程输出、错误定位和暂停滚动。
- Build/Run/Package 还未形成 IDE 内闭环。
- 主题切换、最近项目和崩溃恢复尚未实现。

## 8. 下一阶段建议

1. 修复 D3D12 截图回读，补齐三后端截图门禁。
2. 用反射 API 生成 Inspector 属性行，接 Number/Drag/Vector/Color/枚举控件。
3. 接 FileDialog、AssetGrid 与资源导入/引用编辑。
4. 接 Gizmo 输入坐标、视口拾取和多选 Transform。
5. 做多文档 Tab、查找替换和 CodeEditor 大文件虚拟化。
6. 接构建进程、Console 流、Problems 跳转和 Game View 独立运行状态。
7. 最后补主题、最近项目、崩溃恢复与布局兼容迁移。
