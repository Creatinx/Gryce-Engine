# GryceUI 实现计划

本文定义 GryceUI 在 Gryce Engine 中的定位、节点继承关系、渲染边界和分阶段实现顺序。

## 1. 目标与边界

GryceUI 是引擎的可选 UI 子系统，面向游戏 HUD、菜单、调试工具和未来的 GryceIDE。

- UI 节点、布局、输入路由和 DrawList 属于 GryceUI。
- UI 最终合成属于渲染器的 UI pass，复用引擎 RHI/`RenderContext`，不另建 OpenGL / Vulkan / D3D12 三套 UI 后端。
- GryceIDE 的窗口、面板、文档、选择状态和编辑器命令属于 IDE 项目，不进入引擎。
- 引擎内置的 FPS 叠加与 F1 ImGui 调试窗口保持独立，不依赖 GryceUI。
- 第一阶段采用保留模式，不并行维护保留模式和立即模式两套 API。

## 2. 节点层级

当前基础层级：

```text
Component
└── Node
    ├── Node3D
    └── Node2D
        └── NodeUI
```

- `Node` 提供节点域和节点级可见性，不另存 Transform。
- `Node3D` 表示 3D 场景节点。
- `Node2D` 表示 2D 场景节点，提供 `z_index` / `top_level`。
- `NodeUI` 继承 `Node2D`，提供 UI 尺寸、输入接收、焦点能力和裁剪开关。
- `Component2D` 继续表示 2D 绘制特性，不参与节点身份继承。一个实体应有一个节点身份，再按需挂 Sprite2D、Label、Collider2D 等组件。

UI 树建议挂在一个实体下的组件树中：

```text
Entity
└── NodeUI root
    ├── NodeUI panel
    │   ├── NodeUI label
    │   └── NodeUI button
    └── NodeUI overlay
```

这样控件不需要各自占用一个 ECS Entity；实体级父子关系和组件树父子关系也不会形成两套互相竞争的 UI 场景树。

## 3. 模块划分

建议新增以下模块，但仍由现有渲染器完成最终提交：

```text
components/node_ui.h      NodeUI 节点与布局字段
components/ui_root.h      UI 根和 UIRuntime 生命周期
engine/ui/ui_runtime.*    UI 树遍历、布局、命中、焦点和事件冒泡
engine/ui/ui_input.h      平台无关输入与事件模型
engine/ui/ui_drawlist.h   Rect / Text / Image / Clip 命令
engine/ui/ui_widgets.h    基础控件
engine/ui/ui_extended_widgets.h  容器、停靠、弹层、拖放
engine/ui/ui_editor_widgets.h    IDE 所需完整控件与文档视图

engine/render/
└── ui_pipeline.*         DrawList -> IRenderer2D overlay
```

第一版不拆成独立 DLL。先在 `GryceCore` / `GryceRenderer` 内部建立稳定模块边界；只有出现独立分发或多个宿主真实需要时再拆库。

## 4. 分阶段计划

### 阶段 0：节点基础（已完成）

- 新增 `Node`、`NodeUI`。
- `Node3D` / `Node2D` 改为继承 `Node`；`NodeUI` 继承 `Node2D`。
- 节点类型进入组件工厂和反射表。
- 可见性同时考虑组件父链和 Entity 父链。
- 覆盖基类查询、类型链、可见性和序列化回归测试。

验收：`component_hierarchy`、`component_query`、`component_serialize` 全部通过。

### 阶段 1：UI 运行时骨架（已完成）

- 定义 NodeUI 根生命周期与树遍历。
- 增加矩形、最小尺寸、锚点和屏幕空间坐标映射；世界空间 UI 留到阶段 5。
- 先实现 `Manual`、`Column`、`Row` 三种布局，不提前实现通用约束求解器。
- 实现鼠标命中测试、hover/active/focus 状态和基础键盘事件。
- 将 `NodeUI` 树转换为稳定的 `UIDrawList`。

本阶段的实现入口：

```text
engine/ui/ui_geometry.h    UIRect
engine/ui/ui_runtime.*     NodeUI 树布局、命中、焦点、事件冒泡、DrawList 构建
engine/ui/ui_drawlist.h    Rect/Text/PushClip/PopClip 命令
engine/ui/ui_input.h       平台无关的 UIInputFrame / UIEvent
engine/ui/ui_widgets.h     Phase 1 最少的 UIPanel / UILabel
```

验收（已通过 `GryceUiSkeletonTests`）：

- 一个实体可以承载多级 NodeUI 组件树。
- 尺寸、可见性、裁剪和输入命中具有确定性。
- 不创建 GPU 资源即可在无头测试中验证布局和 DrawList。
- 鼠标、键盘事件沿 NodeUI 组件父链冒泡，`focusable` 节点接收键盘和字符事件。
- Column/Row 布局、padding、gap、最小尺寸和归一化锚点具有固定结果。

### 阶段 2：UI 渲染管线（已完成）

- 实现 `UIRenderPass`，在 3D 与 Canvas2D 之后提交。
- 支持纯色矩形、纹理四边形、字体字形和矩形裁剪。
- 文本先采用一个字体图集和一次批量上传。
- DrawList 只包含后端无关数据；GPU 资源由渲染器持有。
- 所有 GPU 写入走现有 `RenderContext` 命令缓冲。

验收：

- OpenGL 后端先通过截图测试；Vulkan / D3D12 跟随现有 RHI 能力接入。
- UI 不抢占或重复创建图形后端。
- UI pass 关闭时输出与当前版本一致。

实际实现：`engine/render/ui_pipeline.*` 将 UIDrawList 翻译到现有 `IRenderer2D`
overlay pass；`UIRoot` 由 `Canvas2DPipeline` 自动发现，排在 3D/2D 之后提交。
输入在 `GRender_BeginFrame` 采样一次，避免多个视图重复消费字符和滚轮事件。

### 阶段 3：基础控件（已完成）

- `Panel`
- `Label`
- `Image`
- `Button`
- `CheckBox`
- `Slider`
- `TextInput`
- `ScrollView`

控件只依赖 NodeUI 布局、输入和 DrawList，不直接调用引擎组件或编辑器状态。

当前已实现 `UIPanel`、`UILabel`、`UIImage`、`UIButton`、`UICheckBox`、
`UISlider`、`UITextInput`、`UIScrollView`。按钮、勾选框和滑条支持键盘操作；
TextInput 支持 UTF-8、光标移动、Backspace/Delete 和 IME 预编辑。

验收：

- 每个控件具备 hover / pressed / disabled / focused 状态。
- 键盘焦点、Tab 导航、文本编辑和裁剪区域可自动测试。
- 控件不依赖特定 IDE 面板。

### 阶段 4：GryceIDE 接入（已完成最小可运行宿主）

- 新建 GryceIDE 宿主，GryceUI 负责界面，GryceEngine C API 负责场景和资源操作。
- IDE 通过 adapter 把选择、场景树、属性和资源事件映射为 UI 数据。
- 第一版使用独立 IDE 主窗口加引擎子窗口；需要纹理级合成时再接引擎离屏视口。
- 面板布局、停靠、快捷键和命令历史属于 IDE，不属于 GryceUI。

验收：

- IDE 能打开项目、显示场景树、编辑 Transform 属性和预览 3D 视口。
- GryceIDE 不进入引擎 DLL；GryceUI 作为引擎内的 UI 子系统存在，渲染器只负责最终 overlay 提交。

实际入口为 `ide/ide_main.cpp`，构建产物 `GryceIDE.exe`。当前支持命令行打开
项目和场景、真实父子 Hierarchy、实体选择/创建/删除/复制/重命名、Transform
编辑与 Apply、Undo/Redo、Play/Pause/Step/Stop、Reload、布局恢复，以及面板内
3D Scene、2D Scene、3D + 2D 分屏和 Game View。UI 本身仍由 `UIRoot` 自动发现，
通过单一 `UIDrawList -> IRenderer2D overlay` 路径提交。

### 阶段 5：性能与扩展（骨架能力已完成）

- 合批、图集扩容、脏矩形和按需重建。
- 字体 fallback、HiDPI、IME 和多语言文本。
- 世界空间 UI 根：通过当前 3D 相机的 view-projection 把 `UIRoot` 投影到屏幕，并按深度计算布局缩放。
- 只有在第二个真实宿主需要时，才增加独立后端抽象或动态模块。

当前已加入 DrawList 缓存与显式 `invalidate()`、按最大 UI 字号触发的字体栅格
档检查、HiDPI 指针缩放、UTF-8 与 IME 预编辑。合批继续使用 Renderer2D 已有
批次；没有为 UI 重造 GPU 后端。`UIRootSpace::World` 使用当前 3D 相机投影，
屏幕空间仍是默认路径。

## 5. 每阶段测试门禁

| 阶段 | 必测内容 |
| --- | --- |
| 0 | 节点基类查询、NodeUI 类型链、Entity/组件树可见性、序列化 |
| 1 | 布局确定性、坐标映射、命中测试、焦点与事件冒泡 |
| 2 | DrawList 顺序/裁剪/DrawCall 数、单后端截图、关闭 UI 的零回归 |
| 3 | 控件状态机、键盘/文本输入、滚动与裁剪 |
| 4 | IDE 场景打开、属性编辑、撤销重做、3D 视口嵌入 |
| 5 | 帧时间预算、批次数、字体图集占用、长时间运行稳定性 |

## 6. 暂不实现

- 四套独立 UI 图形后端。
- 完整 CSS/约束布局引擎。
- 每个控件对应一个 ECS Entity。
- 引擎内置 IDE、停靠系统和文档模型。
- 未出现真实消费者前的 UI 热重载、动画时间轴和主题市场。

这些能力都可以在骨架稳定后增量加入，但不能先于实际控件和宿主需求出现。
