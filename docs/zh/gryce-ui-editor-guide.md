# 用 GryceUI 搭建编辑器：实操指南

本文是一份"如何从零搭一个基于 GryceUI 的编辑器"的实操指南。它面向已经熟悉
GryceEngine 基本概念（实体、组件、ECS、渲染后端）的开发者，重点讲清楚编辑器
宿主（host）与引擎之间的接线方式、UI 树如何编写、场景如何嵌入面板、编辑操作
如何下发与撤销。

配套参考：架构调查报告 `gryce-ide-implementation-report.md`、
完整化路线 `gryce-ide-roadmap.md`（仅作愿景参考，不要求一次到位）、
`ide/ide_main.cpp`（本文所有接线片段均来自这个最小可运行宿主）。

---

## 1. 定位与边界

编辑器宿主只做三件事，不要把它塞回引擎：

- **持有编辑状态**：当前选中的实体、打开的场景路径、面板布局、撤销上下文。
- **把用户意图翻译成编辑命令**，通过唯一的写入通道下发（见第 11 节）。
- **把引擎渲染出来的画面（3D/2D 离屏纹理、Gizmo）嵌进 UI 面板**。

引擎 DLL（`GryceCore` / `GryceRenderer`）**不反向依赖宿主**。UI 控件、布局、
命中、输入、动画全部由 GryceUI 在宿主进程内完成，跨后端（OpenGL / Vulkan /
D3D12）只走一套 `IRenderer2D` overlay 代码。

---

## 2. 总体架构（数据流向）

```text
NodeUI 控件树（挂在某个 Entity 的 UIRoot 组件下）
        │
        ▼  每帧由引擎 canvas2d_pipeline 自动驱动
UIRuntime（布局 / 命中 / 焦点 / 滚动 / 拖放 / 命令 / 动画推进）
        │
        ▼
UIDrawList（后端无关的绘制命令）
        │
        ▼
render_ui_draw_list → IRenderer2D overlay → GL / Vulkan / D3D12
```

关键结论：**你不需要自己每帧调 `runtime.update()` 或 `build_draw_list()`。**
只要场景里存在一个 `UIRoot` 组件，引擎在 `GRender_EndFrame()` 阶段会自动：
收集所有 `UIRoot` → 用 `capture_ui_input()` 抓取输入 → `runtime().update(input)`
→ `build_draw_list()` → `render_ui_draw_list()`。宿主只负责把 `UIRoot` 挂上去、
建好控件树、跑帧循环。

---

## 3. 最小宿主骨架

下面是 `ide_main.cpp` 主循环的精炼版，去掉了命令面板 / 菜单等枝节：

```cpp
#include "api/core_api.h"
#include "api/render_api.h"
#include "api/window_api.h"   // GWindow_*
#include "api/input_api.h"    // GInput_SyncToCore
#include "components/ui_root.h"
#include "components/node_ui.h"

int main(int argc, char** argv) {
    // 1) 引擎核心：编辑器不自动加载主场景
    GCore_SetAutoLoadMainScene(false);
    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project_root.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) return 1;

    // 2) 窗口 + 渲染后端（api 取 opengl|vulkan|dx12）
    GWindow_CreateEx("MyEditor", width, height, GWINDOW_MODE_WINDOWED, api);
    GRender_SetViewportOutput(true);   // 关键：世界渲染到离屏纹理而非窗口
    GRenderInitDesc rd{};
    rd.version = sizeof(GRenderInitDesc);
    rd.native_window = GWindow_GetRenderHandle();
    rd.api = api;
    rd.viewport_w = width; rd.viewport_h = height;
    rd.sync_mode = false;
    if (GRender_Init(&rd) != 0) return 1;

    // 3) 建立 UI 树（见第 4 节），根是一个 UIRoot 组件
    auto* owner = /* 你的根实体 */;
    auto* root = owner->add_component<gryce_engine::components::UIRoot>();
    build_ui(root, width, height);

    // 4) 帧循环
    while (!GWindow_ShouldClose()) {
        GWindow_PollEvents();
        GInput_SyncToCore();                 // 把 OS 输入同步进引擎
        int w = width, h = height;
        GWindow_GetSize(&w, &h);
        if (w != width || h != height) { width = w; height = h; layout_editor(root, w, h); }

        const float dt = frame_dt();
        GCore_BeginFrame(dt);                // 消费上一帧排队的编辑命令
        update_viewport_texture(root);       // 把最新离屏纹理交给 UIViewportPanel
        GRender_BeginFrame();
        GRender_RenderWorld();               // 渲到离屏纹理（ViewportOutput 模式）
        GRender_EndFrame();                  // 引擎在此自动驱动 UI runtime + 绘制
        GCore_EndFrame();
    }
    GRender_Shutdown(); GWindow_Destroy(); GCore_Shutdown();
    return 0;
}
```

注意：`GRender_SetViewportOutput(true)` 必须在 `GRender_Init` 之前一次性打开。
运行期切换会触发管线热重载，有一两帧卡顿。**这是编辑器把 3D 画面嵌进面板的
前提**（否则引擎直接把画面打到窗口上，面板里就没有可采样的纹理）。

---

## 4. 构建 UI 树：UIRoot 与 NodeUI

`UIRoot` 是一个挂在实体上的组件，它内部持有一个 `UIRuntime`：

```cpp
class UIRoot : public NodeUI {
public:
    ui::UIRuntime& runtime();          // 引擎每帧自动驱动它
    // 序列化字段：active / accepts_input / space(Screen|World) / world_scale
};
```

- 所有界面元素都是 `NodeUI` 的子类节点，作为 `UIRoot` 的子节点挂在组件树上。
- 控件树与 ECS 实体树**职责分开**：UI 树只描述界面，不创建游戏实体。
- `NodeUI` 提供布局原语：`size` / `min_size` / `anchor_min` / `anchor_max`
  （归一化锚点相对父内容区）、`layout_mode`（Manual/Column/Row）、`padding` /
  `gap`、`clip_contents`（裁剪子节点）、`receives_input` / `focusable`。

尺寸约定：UI 用**左上原点、像素、y 向下**的统一坐标系。后端（包括 OpenGL 的
`glScissor` 左下原点）在内部做坐标转换，控件与 `Renderer2D` 不需要关心。

---

## 5. 现成控件清单

`engine/ui/ui_widgets.h` 已提供一组可直接用的控件，都是 `NodeUI` 子类：

| 类 | 用途 |
|----|------|
| `UIPanel` | 容器面板（带主题背景与边框） |
| `UILabel` | 文本标签 |
| `UIImage` | 贴图显示 |
| `UIViewportPanel` | **把引擎离屏纹理（3D/2D/Game）显示成面板内容** |
| `UIButton` | 按钮（自带 hover/press 主题过渡） |
| `UICheckBox` | 复选框 |
| `UISlider` | 滑条 |
| `UITextInput` | 文本输入 |
| `UIScrollView` | 可滚动容器 |

`UIViewportPanel` 是编辑器的核心：它持有一个 `GRenderTextureInfo`
（`index` / `generation` / `width` / `height`），引擎每帧用最新的离屏纹理
刷新它（见第 9 节）。

---

## 6. 自定义控件：继承 NodeUI

当现成控件不够用时，继承 `NodeUI` 并覆写两个虚函数：

```cpp
class MyWidget : public gryce_engine::components::NodeUI {
public:
    // 输入事件。返回 true 表示已处理、阻止继续冒泡给父节点。
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEvent::PointerDown && hit_test_local(event)) {
            pressed_ = true;
            set_pressed_state(true);     // 启动 press 补间
            return true;
        }
        if (event.type == ui::UIEvent::PointerUp) {
            pressed_ = false;
            set_pressed_state(false);
            return true;
        }
        return false;
    }

    // 模型层绘制：只往 draw_list 推后端无关命令，不碰 GPU。
    void add_draw_commands(const ui::UIRect& rect,
                           ui::UIDrawList& draw_list) const override {
        // 用主题 token 取色，fallback 仅在 role==Custom 时生效
        const uint32_t bg = themed_color(background_role, ui_color(0x2a2d33ff));
        draw_list.rect(rect.x, rect.y, rect.w, rect.h, bg, corner_radius);
        // ... 文字、边框等
    }
};
```

要点：

- `themed_color(role, fallback)`：当 `role == UIThemeColor::Custom` 时用
  fallback，否则查当前主题。**控件只引用语义角色，不写死颜色**。
- 动画：激活态用 `set_hovered_state(bool)` / `set_pressed_state(bool)` 启动补间，
  绘制时用 `hover_amount()` / `press_amount()`（0..1）做颜色/位移插值，不要自己
  维护布尔然后硬切。
- `advance_animations(dt)` 由 `UIRuntime` 在每帧 `update` 时递归调用，你通常
  不需要手动调。

---

## 7. 主题系统（语义 token）

颜色与度量都是**语义 token**，不是具体色值。改主题即改 token，所有引用该 token
的控件自动刷新。

枚举（`engine/ui/ui_theme.h`）：

- `UIThemeColor`：`Window` / `Panel` / `PanelRaised` / `Input` / `Text` /
  `TextMuted` / `Border` / `Accent` / `AccentHover` / `AccentPressed` /
  `Selection` / `Success` / `Warning` / `Error` / `Overlay` / `Track` / `Custom`。
- `UIThemeMetric`：`Padding` / `Gap` / `RowHeight` / `ControlHeight` /
  `BorderWidth` / `FontSize` / `SmallFontSize`。
- `UIThemePreset`：`Dark` / `Light` / `HighContrast` / `Nord`。

公开函数：

```cpp
uint32_t ui_theme_color(UIThemeColor c);
float    ui_theme_metric(UIThemeMetric m);
uint32_t ui_theme_revision();                 // 每次改动 +1

void set_ui_theme_preset(UIThemePreset p);    // 切内置预设
void set_ui_theme_color(UIThemeColor c, uint32_t rgba);
void set_ui_theme_metric(UIThemeMetric m, float v);
int  load_ui_theme_json(const char* json);    // 返回 1/0
int  load_ui_theme_file(const char* path);

uint32_t ui_theme_rgba(float r, float g, float b, float a = 1.0f);
uint32_t ui_theme_mix(uint32_t from, uint32_t to, float amount);  // 逐通道插值
```

工作机制：`set_*` 任意改动都会 `bump_revision()`。`UIRuntime` 每帧比对
`ui_theme_revision()`，发现变化就 `invalidate()` 重算布局/绘制缓存。**所以换肤
是实时的，不需要手动刷新每个控件。**

---

## 8. 动画系统（hover / press 过渡）

基础按钮的完整过渡链路（来自 `UIButton`）：

```cpp
// 默认角色
background_hover_role   = UIThemeColor::AccentHover;
background_pressed_role = UIThemeColor::AccentPressed;

// 绘制时
uint32_t base  = themed_color(background_role, ui_color(0x3a3f4bff));
uint32_t hover = themed_color(background_hover_role, base);
uint32_t press = themed_color(background_pressed_role, hover);
uint32_t bg = ui_theme_mix(base,  hover, hover_amount());
bg         = ui_theme_mix(bg,    press, press_amount());
```

- `set_hovered_state(true/false)` 启动 hover 补间（时长 `hover_duration`，默认
  0.12s）；按下用 `press_duration`（默认 0.08s），释放用更短的 0.7 倍时长。
- 补间曲线默认 `UIEasing::OutCubic`。
- 同一帧内重复回放同一输入帧不会重复派发事件：`UIRuntime::update` 用
  `frame_serial` 整体去重（这是早期一个真实 bug 的修复点——之前漏过去重会导致
  一次点击被派发两次）。

---

## 9. 把 3D / 2D 场景嵌进 UI 面板

流程分三步：

1. **打开离屏输出**：`GRender_SetViewportOutput(true)`（init 前）。
2. **每帧取最新纹理句柄**，交给 `UIViewportPanel`：

```cpp
void update_viewport_texture(components::UIRoot* root, UIViewportPanel* panel) {
    GRenderTextureInfo info{};
    if (!GRender_GetViewportTextureInfo(&info)) return;     // 3D 视口
    // 2D 场景用 GRender_GetCanvas2DViewTextureInfo
    // Game 视口用 GRender_GetGameViewTextureInfo
    if (panel->texture.index == info.index &&
        panel->texture.generation == info.generation &&
        panel->texture.width == info.width &&
        panel->texture.height == info.height) return;       // 没变就别 invalidate
    panel->texture = {info.index, info.generation, info.width, info.height};
    root->runtime().invalidate();                           // 让绘制缓存重算
}
```

3. **视口里的 Gizmo 坐标原点**：Gizmo 画在窗口前景，坐标是窗口绝对像素；而视口
   面板只知道自己的 `vw×vh`。用 `GViewport_SetScreenOrigin(x, y)` 告诉引擎视口在
   窗口的哪个位置，之后 `GRender_GizmoHitTest` 的入参就是窗口绝对坐标（与鼠标、
   与 UI 一致）。不设置则原点为 0（等价于视口铺满窗口）。

编辑器相机用 `GViewport_SetEditorTransform(px,py,pz, qx,qy,qz,qw, fov, near, far)`
（四元数形式，避免 yaw/pitch 约定不一致导致画面与拾取差 90°）。

---

## 10. 变换 Gizmo（编辑器手柄）

Gizmo 作用于当前选中实体（`GEdit_SelectEntity` 设置的选择状态），屏幕空间绘制。
宿主在视口面板内完成交互：

```cpp
const int axis = GRender_GizmoHitTest(sx, sy);   // 0/1/2=X/Y/Z, 3=中心, -1=未命中
if (pointer_down && axis >= 0) GRender_GizmoBeginDrag(axis);
if (dragging)                 GRender_GizmoDrag(delta_px);  // 沿轴屏幕位移(像素)
if (pointer_up)               GRender_GizmoEndDrag();        // 整段拖拽记一步撤销
```

- `operation`：`0=平移 1=旋转 2=缩放`；`space`：`0=世界 1=局部`，用
  `GRender_SetGizmoMode(op, space)` 设置。
- `GRender_GizmoDrag` 返回 1 表示已下发修改——**修改走编辑命令面**
  （`ECMD_GIZMO_MANIPULATE`），因此撤销/重做与 Inspector 数值编辑完全一致。
- 整段拖拽会被引擎记成一步撤销（内部 `BeginEdit/EndEdit` 分组）。

---

## 11. 与引擎通信：唯一写入通道

所有编辑操作都通过编辑命令面下发。引擎在 `GCore_BeginFrame` 边界统一消费命令，
从而把"UI 线程的编辑意图"与"场景状态"解耦，撤销/重做、Play 快照、批量编辑
都只需在一个地方拦截。

优先用语义化封装（`engine/api/edit_api.h`）：

```cpp
GEdit_CreateEntity(name, parent);          // parent=0 表示场景根
GEdit_DestroyEntity(handle);
GEdit_RenameEntity(handle, name);
GEdit_ReparentEntity(handle, parent);
GEdit_SelectEntity(handle);
GEdit_SetTransform(handle, &pos, &rot, &scale);   // 一次性写局部 TRS
GEdit_TranslateEntity(handle, &delta);            // 增量平移，同帧可叠加
GEdit_SetProperty(handle, type_hash, "albedo", &value, sizeof(value));
GEdit_AddComponent(handle, "MeshRenderer");
GEdit_RemoveComponent(handle, type_hash);
GEdit_SetMaterial(handle, mesh_path, r,g,b, roughness, metallic);
GEdit_SetRenderTargetSize(target, w, h);  // target: 0=场景 1=游戏
GEdit_SaveScene(path);
GEdit_ImportAsset(path);
GEdit_SetPlayMode(mode);   // 0=Stop 1=Play 2=切换暂停
GEdit_StepFrame(frames);
```

注意点：

- 这些函数**立即返回**，返回值是"是否入队成功"，不代表执行结果；执行期失败
  只写日志、不回调。
- `GEdit_SetTransform` 是绝对值、入队时即补齐缺省分量；同一帧"先设位置再微调"
  必须用 `GEdit_TranslateEntity`（增量，执行时才读当前值，可正确叠加）。
- 更底层是 `GCore_PushCommand(const GCommand*)`，命令 payload 布局见 `edit_api.h`
  里的 `GCmd*` 结构体（`GCmdSetTransform` / `GCmdSetProperty` / `GCmdGizmoManipulate`
  等），定长数组、超长截断。需要批量或自定义命令时直接拼 payload 走这条。

---

## 12. 撤销 / 重做

快照式：每条变更类命令执行前抓一份场景 JSON 入栈，撤销即反序列化回来。实体
句柄按 UUID 保留，宿主持有的选择与面板状态不需要重建。

```cpp
int r = GCore_Undo();   // 1=执行了撤销 0=无可撤销
int r = GCore_Redo();   // 1=执行了重做 0=无可重做
GCore_GetUndoCount(); GCore_GetRedoCount();
GCore_ClearUndoHistory();
GCore_SetUndoLimit(limit);        // 0=禁用记录；默认 64
GCore_BeginEdit("move entity");   // 连续编辑分组：抓快照
// ... 多条 GEdit_* 命令 ...
GCore_EndEdit();                  // 整段算一步
```

约束：**Play 模式下撤销栈冻结**（播放期的物理/动画改动不入编辑历史）。拖拽类
连续编辑务必用 `BeginEdit/EndEdit` 包起来，否则每一步都进历史、撤销时要按很多次。

---

## 13. 读回引擎状态

不要直接轮询场景，用回调 + 内部指针：

```cpp
GCore_RegisterCallback_OnEntitySelected([](GEntityHandle h){ /* 刷新 Inspector */ });
GCore_RegisterCallback_OnEntityDeselected(...);
GCore_RegisterCallback_OnSceneLoaded(...);
GCore_RegisterCallback_OnPlayModeChanged(...);
GCore_RegisterCallback_OnEntityListChanged(...);   // 增删实体 → 刷新 Hierarchy
GCore_RegisterCallback_OnComponentChanged(...);    // 属性变化 → 刷新面板
GCore_RegisterCallback_OnLogMessage(...);          // Console 输出
```

同进程内要做深度内省时，用 `GCore_GetInternalWorldPtr()` 拿到不透明的 `World*`
（模块内部再 cast），读 ECS 存储、反射字段等。

---

## 14. 输入与坐标约定

- 输入流：宿主每帧 `GWindow_PollEvents()` + `GInput_SyncToCore()`，引擎
  `capture_ui_input()` 把 OS 输入转成 `UIInputFrame`，驱动所有 `UIRoot`。宿主
  **不需要手动构造 `UIInputFrame`**。
- 同一输入帧（相同 `frame_serial`）由 `UIRuntime` 去重，重复回放不会重复派发事件。
- 视口/Gizmo 坐标：用 `GViewport_SetScreenOrigin` 声明视口在窗口的位置，Gizmo
  命中测试即收窗口绝对坐标。
- 离屏视口 + UI 组合必须保证 UI 是 Canvas2D 场景之后的**独立 overlay 层**（D3D12
  下由 `prepare_gryce_ui_overlay` 在 `begin_overlay_pass` 内把 present 目标翻转
  进真实 back buffer）。低层不要自己重排 overlay 顺序。

---

## 15. 防踩坑清单（来自实战）

1. **DX12 截图/出图黑屏**：`back_buffer_as_rt_` 状态机——使用 present 目标时
   若误置真，帧末翻转 blit 会跳过真实 back buffer 的 PRESENT→RT 转换，回读全黑。
   离屏 `bind_framebuffer` 必须把它置 false。
2. **UI 在 DX12 下静默消失**：`Renderer2D::flush_batch` 通用路径调 `IMesh::draw()`，
   而 `D3D12Mesh::draw()` 是空实现。D3D12 的 2D 批次必须走
   `D3D12Renderer2D::submit_mesh` → `D3D12Backend::draw_mesh(mesh_handle,
   shader_handle)`；`configure_batch_mesh` 对 `D3D12Mesh` 要
   `set_streaming_vertices(true)`。
3. **头文件命名空间**：D3D12 的 2D 实现文件在 `gryce_engine::render`，`D3D12Mesh`
   在 `gryce_engine::render::d3d12`，必须写 `dynamic_cast<d3d12::D3D12Mesh*>`。
4. **无头验证用 Vulkan**：本机无显示会话里 Vulkan 的 WSI/GLFW 能初始化并出图，
   DX12 交换链需要真实显示。要验证 GryceUI 离屏出图优先
   `--api vulkan --editor-preview --ui-demo --shot`。
5. **`GRender_SetViewportOutput` 必须 init 前打开**，否则场景直接渲到窗口，
   面板里没有可采样纹理。
6. **主题/布局改动要 `invalidate()`**：手动改了 `UIRoot` 的树或纹理后，调
   `root->runtime().invalidate()` 让缓存重算，否则下一帧还是旧画面。
7. **测试门禁不含已被删除的 API**：旧的 `GUi_*` 内建 UI API、`test/ui_tests.cpp`、
   `assets/themes/*.json` 已在重构中删除；对应的 `GryceUiTests.exe` 是陈旧孤立
   产物，不在 `ctest` 门禁内，跑它失败与当前代码无关，直接删掉即可。

---

## 16. 参考文件

| 文件 | 内容 |
|------|------|
| `engine/components/node_ui.h` | `NodeUI` 基类：布局/输入/主题角色/动画补间 |
| `engine/components/ui_root.h` | `UIRoot`：持有 `UIRuntime` 的 UI 根组件 |
| `engine/ui/ui_runtime.h` | `UIRuntime`：布局、命中、焦点、命令、动画 |
| `engine/ui/ui_theme.h` / `.cpp` | 主题 token、预设、JSON、运行时 revision、mix |
| `engine/ui/ui_widgets.h` | 现成控件（Panel/Button/ViewportPanel/...） |
| `engine/api/edit_api.h` | `GEdit_*` 编辑命令封装与 payload 布局 |
| `engine/api/core_api.h` | `GCore_*` 核心 API、撤销/重做、命令通道 |
| `engine/api/render_api.h` | `GRender_*` 渲染控制、视口纹理、Gizmo、离屏输出 |
| `engine/api/viewport_api.h` | `GViewport_*` / `GGameView_*` 视口与编辑器相机 |
| `engine/render/canvas2d_pipeline.cpp` | 每帧驱动所有 `UIRoot` 的渲染落点 |
| `ide/ide_main.cpp` | 最小可运行编辑器宿主（本文所有接线片段来源） |
