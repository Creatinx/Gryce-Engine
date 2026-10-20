# 内置 UI 接口（ui-api）

引擎把 Dear ImGui 编进 `GryceRenderer.dll`，并用 `api/ui_api.h` 把控件面导出成纯 C。宿主与扩展只调 `GUi_*`，**不 include `imgui.h`**。本文说明为什么必须这样、什么时候能调、以及几个坐标系约定。

相关文档：[editor-api.md](editor-api.md)（视口输出与 `GRender_SetUICallback`）、[extension.md](extension.md)（扩展 ABI）、[app-entry.md](app-entry.md)（宿主启动顺序）。

## 1. 为什么必须走 C 接口

ImGui 的上下文、字体图集、临时缓冲都是**进程内全局**变量，而且按 DLL 划分。`imgui.cpp` 编在 `GryceRenderer.dll` 里，扩展若自己 include `imgui.h`：

* 要么链接到扩展自己的一份 `imgui.cpp`——两份 `GImGui` 互不相干，`ImGui::Begin` 塞进扩展的上下文，引擎的 `Render()` 遍历的是引擎的上下文，屏幕上什么都没有；
* 要么只拿到声明、链接期才炸（`GImGui` 是 `extern` 的 `_ThreadLocal` 指针，符号能找到，运行时是空指针）。

前者的表现是**画了但屏幕上什么都没有，且编译、链接都不报错**——这是这一层存在的唯一原因。顺带的好处是 ImGui 版本升级不会波及调用方：`ui_api.h` 用的是自己的一套编号（`GUI_WIN_*`、`GUI_TEXT_*`、`GUI_MOUSE_*`），内部再映射。

## 2. 两道闸

`api/ui_api.cpp` 里每个函数都过两道闸：

| 闸 | 条件 | 行为 |
| --- | --- | --- |
| 编译期 | 没定义 `GRYCE_HAS_IMGUI`（WebGL 构建、`-DGRYCE_ENABLE_IMGUI=OFF`） | 整个实现退化成空函数，链接正常、运行无效果 |
| 运行期 | 不在 `GRender_SetUICallback` 的回调帧内 | 返回 0 / 无操作 |

第二道闸不是防御性编程，是必需的：宿主通常在 `tick` 里顺手算布局尺寸（"视口还剩多少宽"），那些调用和真正的绘制在**不同阶段**。没有这道闸，这些调用会往处于 `Idle` 状态的 ImGui 上下文里塞控件，ImGui 直接断言退出。

```c
// 判断"画了没有"用这个，不要靠返回值猜
int GUi_InFrame(void);

// 窗口尺寸（逻辑像素）
void GUi_GetDisplaySize(float* out_w, float* out_h);
```

`GUi_InFrame()` 在无窗口环境（CI 里跑 `--frames 1`）恒为 0，所以同一份界面代码可以直接跑在无头构建里，不需要 `#ifdef`。

## 3. 回调内不能调什么

`GRender_SetUICallback` 的回调在 `g_renderer.mutex` **已持有**的路径上执行（`render_api.cpp` 的 ImGui 帧中段），所以：

| 禁止 | 原因 | 替代 |
| --- | --- | --- |
| `GRender_*` | 几乎全部内部再加锁 | 在 `tick` 里取好值传进来（纹理 ID、Gizmo 操作、统计数字） |
| `GCore_*` | 命令缓冲、撤销栈都在锁内 | 记成意图，下一帧执行 |
| `GEdit_*` | 上层封装同 `GCore_*` | 同上 |
| `GScene_*` 的修改类 | 同上 | 同上 |
| `GEntity_Get*` 查询 | 读路径也过锁 | 同上，或读一次缓存在面板模型里 |
| `GAsset_ScanDirectory` | 文件遍历 + 缓存写入 | `tick` 里刷新，UI 读缓存 |

一句话：**回调内只做纯绘制，唯一允许的输出是"把用户操作记成一个纯数据结构"**。

### 意图队列

编辑器（`editor/ext/ui/intent.h`）的做法：

```cpp
// 回调里（draw.cpp）
if (GUi_IsItemClicked(GUI_MOUSE_LEFT))
    push_select(handle);          // 只是往 std::vector<Intent> 里塞一条

// 下一帧开头（ui.cpp 的 tick，锁外）
intent::flush();                  // 真正调 commands::select() / view::set_size()
```

代价是**一帧延迟**（点击到生效隔一帧），收益是回调里永远不会死锁。`flush()` 先 `swap` 出队列再逐条执行——执行过程中可能又入队（比如"选中后立刻求一次包围盒"），不能边遍历边追加。

选择类意图（`kSelect` / `kToggleSelect`）在同帧内合并：框选一次点十个物体，只保留最后一个，撤销栈里不会多出十步。

## 4. 控件面

### 容器

```c
GUi_BeginWindow(name, x, y, w, h, flags);   // flags: GUI_WIN_*
GUi_BeginChild(id, w, h, border);           // w/h 传 0 = 填满剩余
GUi_BeginMenuBar(); GUi_BeginMenu(label); GUi_MenuItem(label, shortcut, enabled);
```

`GUI_WIN_NO_SAVED_SETTINGS` 会阻止 ImGui 写 `imgui.ini`。编辑器固定用它——布局一旦被持久化，截图比对和集成测试就不可复现了。

### 控件

| 类别 | 函数 |
| --- | --- |
| 文本 | `GUi_Text` / `GUi_TextColored(color, text)` / `GUi_LabelText` / `GUi_Separator` / `GUi_Spacing` / `GUi_SameLine(spacing)` |
| 按钮 | `GUi_Button` / `GUi_ButtonSize` / `GUi_SmallButton` / `GUi_Selectable(label, selected)` / `GUi_SetItemAllowOverlap` |
| 数值 | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat3` / `GUi_DragFloat4` |
| 文本输入 | `GUi_InputText` / `GUi_SetKeyboardFocusHere` / `GUi_IsItemDeactivatedAfterEdit` |
| 枚举 | `GUi_Combo(label, current, items, count)` |
| 树 | `GUi_TreeNode(label, default_open)` + `GUi_TreePop` / `GUi_CollapsingHeader` |
| 图片 | `GUi_Image(tex, w, h)` |
| 弹出 | `GUi_OpenPopupOnItemClick` / `GUi_OpenPopup` / `GUi_BeginPopup` / `GUi_EndPopup` |
| 布局 | `GUi_SetCursorPos` / `GUi_GetCursorPos` / `GUi_GetContentAvail` / `GUi_SetNextItemWidth` / `GUi_GetTextLineHeight` / `GUi_GetFrameHeight` |
| 命中 | `GUi_LastItemRect` / `GUi_IsItemHovered` / `GUi_IsItemClicked` / `GUi_IsItemActive` / `GUi_IsAnyItemActive` |
| 鼠标 | `GUi_MousePos` / `GUi_MouseDeltaX` / `GUi_MouseDeltaY` / `GUi_IsMouseDown` / `GUi_IsMouseClicked` / `GUi_IsMouseReleased` |
| 捕获 | `GUi_WantCaptureMouse` / `GUi_WantCaptureKeyboard` |
| 样式 | `GUi_PushTextStyle(kind)` / `GUi_PopTextStyle` |

返回值统一：`int` 型控件返回 1 = 本帧被点击 / 值被改过，0 = 没动。就地更新的指针参数（`float*`、`char*`、`int*`）在返回 1 时已经写好新值。

`GUi_Selectable` 用在"整行可点且要显示选中态"的地方（层级树、资产列表）。带右键菜单的行必须紧跟着调 `GUi_SetItemAllowOverlap()`，否则点击会被相邻条目的命中矩形抢走。

## 5. 视口图片与坐标

视口是一张 ImGui 图片，但**图片实际占多大**只有画完才知道（窗口缩放、面板拖动都会变）。`GUi_LastItemRect` 是唯一可靠来源：

```c
GUi_Image(tex, avail_w, avail_h);
float x, y, w, h;
GUi_LastItemRect(&x, &y, &w, &h);   // 绝对坐标，左上角原点
```

拿到矩形后要把它送回引擎，否则两处会错位：

```c
// 在 tick 里（锁外）执行
GViewport_SetScreenOrigin(x, y);    // Gizmo 屏幕投影的偏移
GViewport_SetSize((int)w, (int)h);  // 视口内坐标的除数
```

* **Gizmo** 走的是 `screen_project`，它把 NDC 算成像素后加 `(origin_x, origin_y)`。少了这一步，gizmo 会画在窗口左上角、浮在层级树上面。
* **拾取**（`screen_ray`）用的是视口**内**坐标，即 `mouse_abs - origin`，不需要 `SetScreenOrigin`。
* 两者混用就会得到"看到的在中间、点到的在中下"这种结果。

尺寸变化要顺带 `GRender_InvalidateImGuiTexture(tex)`：渲染目标重建后旧的 `ImTextureID` 指向已释放的纹理。

## 6. 布局约定

`ui_api.h` **不提供 docking**。编辑器用"一整块主窗口 + 固定分区"：

```
菜单栏
工具栏
├─ 层级 236 px ─┬─ 视口（填满） ─┬─ 检视器 306 px ─┤
└──────────────── 资产 / 统计 168 px ───────────────┘
```

理由是 docking 的布局会被 `imgui.ini` 持久化并随用户拖拽漂移，截图比对和 `editor_selftest` 就不可复现。固定分区还能保证"任何窗口尺寸下视口都拿到确定的矩形"，便于写断言。

## 7. 连续编辑与撤销分组

拖拽一次滑块会产生几十帧"值改过"，但那应该只算**一步**撤销。判据是"控件是否还处于活动态"：

```c
// 检视器里改一个数值
if (GUi_IsAnyItemActive()) {
    if (!g_edit_open) { GCore_BeginEdit("Inspector"); g_edit_open = true; }
    GEdit_SetProperty(e, type, name, &v, sizeof(v));
} else if (g_edit_open) {
    GCore_EndEdit();
    g_edit_open = false;
}
```

`GUi_IsAnyItemActive()` 为真时**不能**收分组——收了之后下一次拖拽会另起一步，一次拖拽被切成几十步。注意它在输入框里也返回真（输入框聚焦算活动态），所以"打字改名字"同样会合并成一步，这正是想要的。

## 8. 与宿主输入的分工

UI 吃掉鼠标/键盘时，宿主的场景导航与快捷键必须让路：

```c
// 视口拖拽只在自己被 hover 且 UI 没吃鼠标时启动
if (!GUi_WantCaptureMouse() && GUi_IsItemHovered()) { ... }

// 快捷键派发前拦一道：否则在名字输入框里打 "Wire" 会顺手中途切走移动/旋转/缩放
if (GUi_WantCaptureKeyboard()) return;   // 不派发任何快捷键
```

漏掉键盘这一道的典型表现是：重命名实体时按 `W`，物体开始平移；按 `R`，切到旋转工具且名字里少一个字母。

## 9. 已知边界

* 无 docking、无多窗口停靠、无 `ImPlot`/节点编辑器一类扩展——需要的话加成 `GUi_*` 的新函数，不要在扩展里引 ImGui。
* 主题只有一套暗色 + 语义文本色（`GUI_TEXT_*`）。要换主题改引擎内 `debug_imgui` 的 style，不要在调用方硬编码颜色。
* `GUi_Image` 不接受自定义 UV / 边框着色，视口只显示完整纹理。
* 中文等非 ASCII 文本依赖引擎加载的字体图集；字体缺失时显示为方块，不会崩。
