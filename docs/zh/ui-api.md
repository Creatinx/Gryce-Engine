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

**回调内只做纯绘制，唯一允许的输出是"把用户操作记成一个纯数据结构"**。

### 意图队列

编辑器扩展的做法：意图队列由一个小结构体 vector 承载，下一帧 `tick` 在锁外逐条 `flush()`：

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
| 数值 | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat2` / `GUi_DragFloat3` / `GUi_DragFloat4` / `GUi_ColorEdit4` / `GUi_ColorButton(label, rgba)` |
| 文本输入 | `GUi_InputText` / `GUi_SetKeyboardFocusHere` / `GUi_IsItemDeactivatedAfterEdit` |
| 枚举 | `GUi_Combo(label, current, items, count)` |
| 树 | `GUi_TreeNode(label, default_open)` + `GUi_TreePop` / `GUi_CollapsingHeader` |
| 图片 | `GUi_Image(tex, w, h)` |
| 弹出 | `GUi_OpenPopupOnItemClick` / `GUi_OpenPopup` / `GUi_BeginPopup` / `GUi_EndPopup` / `GUi_CloseCurrentPopup` |
| 拖放 | `GUi_BeginDragDropSource` / `GUi_SetDragDropPayloadString` / `GUi_EndDragDropSource` / `GUi_BeginDragDropTarget` / `GUi_AcceptDragDropPayloadString` / `GUi_EndDragDropTarget` |
| 布局 | `GUi_SetCursorPos` / `GUi_GetCursorPos` / `GUi_GetContentAvail` / `GUi_SetNextItemWidth` / `GUi_GetTextLineHeight` / `GUi_GetFrameHeight` / `GUi_CalcTextWidth` |
| 命中 | `GUi_LastItemRect` / `GUi_IsItemHovered` / `GUi_IsItemClicked` / `GUi_IsItemActive` / `GUi_IsAnyItemActive` |
| 鼠标 | `GUi_MousePos` / `GUi_MouseDeltaX` / `GUi_MouseDeltaY` / `GUi_IsMouseDown` / `GUi_IsMouseClicked` / `GUi_IsMouseReleased` / `GUi_GetModifiers` |
| 捕获 | `GUi_WantCaptureMouse` / `GUi_WantCaptureKeyboard` |
| 样式 | `GUi_PushTextStyle(kind)` / `GUi_PopTextStyle` |

返回值统一：`int` 型控件返回 1 = 本帧被点击 / 值被改过，0 = 没动。就地更新的指针参数（`float*`、`char*`、`int*`）在返回 1 时已经写好新值。

`GUi_Selectable` 用在"整行可点且要显示选中态"的地方（层级树、资产列表）。带右键菜单的行必须紧跟着调 `GUi_SetItemAllowOverlap()`，否则点击会被相邻条目的命中矩形抢走。

### 标签与控件的宽度分摊

ImGui 的标签画在控件**右边**。所以 `GUi_SetNextItemWidth(-1)` 会让控件吃满整行、把标签挤到面板外面（截图上只剩半个字）。正确写法是用 `GUi_CalcTextWidth` 扣掉标签宽度：

```c
float avail = 0.0f, unused = 0.0f;
GUi_GetContentAvail(&avail, &unused);
float reserve = GUi_CalcTextWidth("Position") + 14.0f;
if (reserve > avail * 0.6f) reserve = avail * 0.6f;   // 长标签最多占六成
GUi_SetNextItemWidth(-reserve);
GUi_DragFloat3("Position", v, 0.02f);
```

不带可见标签的控件（`"##name"`）才该用 `-1`。

### 拖放

payload 只支持字符串。编辑器要传的东西（实体句柄、资产路径）本来就有文本形式，走文本换来两件事：跨 DLL 不依赖任何内存布局，调试时 tooltip 里也能直接看到内容。

```c
// 源：紧跟在条目之后
if (GUi_BeginDragDropSource()) {
    char id[24];
    std::snprintf(id, sizeof(id), "%lld", (long long)handle);
    GUi_SetDragDropPayloadString("GRYCE_ENTITY", id);
    GUi_EndDragDropSource();
}

// 目标：同样是"上一个条目"
if (GUi_BeginDragDropTarget()) {
    char buf[24];
    int r = GUi_AcceptDragDropPayloadString("GRYCE_ENTITY", buf, sizeof(buf));
    if (r == GUI_DRAG_DELIVER) { /* 松手了，buf 有效 */ }
    GUi_EndDragDropTarget();
}
```

`GUi_AcceptDragDropPayloadString` 返回三种状态，区分它们不是多余的：

| 返回值 | 含义 | 典型用途 |
| --- | --- | --- |
| `GUI_DRAG_NONE` | 没有匹配的 payload 悬停 | 什么都不做 |
| `GUI_DRAG_PREVIEW` | 悬停中，尚未松手 | 画高亮、改提示文字 |
| `GUI_DRAG_DELIVER` | 本帧松手交付，`out_buf` 已写入 | 执行真正的操作 |

只认 `DELIVER` 就够用了；区分 `PREVIEW` 是为了在悬停期间给出"这里能放"的反馈。鼠标键固定为左键，ImGui 没有"用其它键拖"的开关。

### 修饰键

```c
const int mods = GUi_GetModifiers();      // GUI_MOD_CTRL / SHIFT / ALT / SUPER
if (mods & GUI_MOD_SHIFT)  ...            // 加选
else if (mods & GUI_MOD_CTRL) ...         // 切换
else ...                                   // 替换
```

层级树用它区分"替换选择 / 加选 / 切换"，和大多数编辑器一致。


## 5. 视口图片与坐标

视口是一张图片（ImGui 轨走 `ImGui::Image`，内置轨走 drawlist 的 sprite 通道），但**图片实际占多大**只有画完才知道（窗口缩放、面板拖动都会变）。`GUi_LastItemRect` 是唯一可靠来源：

```c
// tex 在 tick 里（锁外）按当前 UI 轨取：ImGui 轨用 GRender_GetImGuiTextureId，
// 内置轨用 GRender_GetUiTextureId（见下面「纹理 id 的两套语义」）。
GUi_Image(tex, avail_w, avail_h);
float x, y, w, h;
GUi_LastItemRect(&x, &y, &w, &h);   // 绝对坐标，左上角原点
```

两条轨上 `LastItemRect` 都可靠，`GUi_Image` 之后紧跟 `GUi_IsItemHovered()` 也都拾取得到这张图（ImGui 的 `IsItemHovered` 走 `LastItemData` 的矩形判定，与 id 无关）——视口拾取因此不用分叉写两份。唯一的差别在激活：ImGui 的 `Image` 没有 id，内置轨的图片有（点它会顺带清掉键盘焦点）。

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

### 纹理上下方向

`GUi_Image` 内部按后端自动决定要不要翻 V，调用方**不要**自己传 UV（接口也不提供）。原因是各后端的"纹理第一行"含义不同：

| 后端 | 渲染目标纹理第一行 | 是否翻 V |
|---|---|---|
| OpenGL | 图像的**下**边（GL 原点在左下） | 翻 |
| D3D12 / Vulkan | 图像的**上**边 | 不翻 |

### 纹理 id 的两套语义（按 UI 轨区分）

`GUi_Image(tex, w, h)` 的 `tex` **不是一套通用的 id**，要按当前 UI 轨（`GRender_GetUIBackend` 可查）取对应的那个：

| UI 轨 | 取 id 的函数 | id 的实际内容 |
|---|---|---|
| ImGui（0，默认） | `GRender_GetImGuiTextureId` | 后端 `ImTextureID`：OpenGL = GL 纹理名、Vulkan = `VkDescriptorSet`、D3D12 = SRV 句柄 |
| 内置（1） | `GRender_GetUiTextureId` | 打包的引擎纹理句柄：低 32 位 = RHI `index`、高 32 位 = `generation` |

混用在内置轨下的症状是"图不显示"：句柄解出来是别的槽位或直接无效（`draw_sprite_region` 对无效句柄静默跳过）。两个函数都在锁外（tick）调用——它们内部要拿 `g_renderer.mutex`，UI 回调里调会自锁。

`GRender_GetUiTextureId` 在 ImGui 轨下等价于 `GRender_GetImGuiTextureId`，所以"一份宿主代码两条轨都能跑"仍然成立，只是取 id 的那一行要换成它。

ImGui 的 `Image()` 默认 UV 是 `(0,0)-(1,1)`，语义是"数据的第一行在 v=0"。GL 的渲染目标第一行是下边，直接采样就把画面上下颠倒 —— 症状是地面跑到天上、立方体的受光顶面显示在底边，而面板文字与图标一切正常（文字走字体图集，与渲染目标无关）。

所以"画面反了但文字正常"几乎一定是这一条，不是相机或投影的问题。引擎内部用 `IImGuiBackend::texture_needs_v_flip()` 标记当前后端，`GRender_Init` 建好 ImGui 后端时把它缓存下来，绘制时只读 —— UI 回调运行在渲染器锁内，回调里问渲染器会二次加锁而死锁。

## 6. 布局约定

`ui_api.h` **不提供 docking**。需要多分区的工具界面时，用"一整块主窗口 + 固定分区"：

```
菜单栏
工具栏
├─ 层级 236 px ─┬─ 视口（填满） ─┬─ 检视器 306 px ─┤
└──────────────── 资产 / 统计 168 px ───────────────┘
```

理由是 docking 的布局会被 `imgui.ini` 持久化并随用户拖拽漂移，截图比对与脚本断言就不可复现。固定分区还能保证"任何窗口尺寸下视口都拿到确定的矩形"，便于写断言。

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

### 拖拽控件必须自己存一份值

引擎里的值要**下一帧**才反映修改（写入走命令队列）。如果每帧都拿面板模型里的值去初始化 `DragFloat`，拖拽过程中被拉回去的永远是上一帧的旧值，手感是"拖一下弹回去"。

```c
// 只在"换了目标"或"没有控件在活动态"时从模型同步
if (changed_target || !GUi_IsAnyItemActive()) {
    buf[0] = model_value;
}
if (GUi_DragFloat("Speed", buf, 0.01f)) { push(Intent{...}); }
```

缓冲要跨帧存活（放在面板状态结构里），不能是栈上的局部变量。

## 7.5 操作队列与受控执行

UI 的刷新率不需要跟渲染帧率一样高，所以"UI 想改引擎状态"不当场执行：回调内只入队，宿主在锁外按策略受控排出。

```c
// 回调内（持锁，只能写纯数据）
GUiOp op = { 0 };
op.type = UI_OP_SET_TRANSFORM;
op.key  = entity_handle;      // 同 (type,key) 只保留最新值
op.vec[0] = x; op.vec[1] = y; op.vec[2] = z;
GUi_PushOp(&op);              // 拖拽每帧都调，排队期间自动塌缩成一条

// 宿主的 tick（锁外）——排出来的操作才能转成 GEdit_* / GCore_*
GUiOp out[32];
int n = GUi_DrainOps(out, 32);
for (int i = 0; i < n; ++i) apply(out[i]);   // apply 里可以放心调引擎 API
```

**合并语义**：`key == 0` 是离散操作（点击、创建、删除），绝不合并；`key != 0` 是连续编辑项（拖拽中的变换），排队期间同键只保留最新值——一次拖拽几十帧只会留下最后一条，撤销栈里就是一步。合并是**就地覆盖**：FIFO 位置与 `seq` 都不变。

**受控执行**由策略控制（`GUi_SetOpPolicy`）：

| 参数 | 含义 |
| --- | --- |
| `max_per_drain` | 单次排出上限；`<=0` 不限制。UI 操作批量落地，不跟渲染帧率挂钩 |
| `min_frames_between_drain` | 两次排出的最小间隔帧数；`<=0` 每个推进过的帧都可排。帧基准由 `GUi_AdvanceOpFrame()` 推进（渲染层每帧自动调，自持帧循环的宿主自己调） |

**满队语义**：容量固定 256。满时只逐出**最旧的可合并项**（拖拽期间每帧都会重推同键，丢一条中间值不丢最终值），离散项一条都不逐；没有可逐出的就拒绝入队并返回 0（同时计入 `stats.rejected`）——"消费方没在排"必须可见，不能被静默吞掉。统计用 `GUi_GetOpQueueStats` 取，`evicted` / `rejected` / `throttled` 三个计数器就是用来排查这类问题的。

**UI 帧节流**（`GRender_SetUiFrameInterval(n)`）是同一思路的另一半：每 `n` 个渲染帧才真正构建一次 ImGui 帧（跑回调与 gizmo），中间的帧直接重绘上一次构建出的 draw data 深拷贝——UI 一直在屏幕上，只是内容按间隔更新，输入也按同一间隔采样。`n <= 1` 为默认（每帧构建）。演示宿主可用 `--ui-interval N` 直接体验。

已用演示宿端到端验证：`--frames 30 --ui-interval 3` 时截图落在第 30 帧（缓存重绘帧），其统计与"每帧构建"的跑法逐字节一致，且与"无 UI"跑法明显不同——节流帧画出的东西与新建帧完全相同。

## 7.6 模型层（`engine/ui/`）与演示宿主

`GUi_*` 是**绘制原语**；跨帧状态、矩形计算、样式、输入门控这些"与画在哪、长什么样无关"的逻辑收在 `engine/ui/`，全部是**不依赖 ImGui 的纯逻辑**——同一份代码能在无窗口 CI 里单测，也能在关掉 ImGui 的构建里编译。

| 文件 | 职责 | 关键规则 |
| --- | --- | --- |
| `ui_id.h` | `(label, seed)` → 64 位稳定 id | 恒非 0（0 是状态存储的空槽标记）；冲突不处置 |
| `ui_state.h` | 跨帧控件状态存储（开放寻址 + 线性探测，删除用后向移位） | 帧号同时是老化基准，超过 `kMaxAgeFrames`（约 900 帧）未被引用的条目被回收；节流 UI 下可见控件每构建帧都在引用，不会误杀 |
| `ui_layout.h` | 确定性布局栈：相同调用序列 → 相同矩形 | `end_row` 按 `max(行高, 行内最高格)` 推进——`cell(0, 30)` 这种显式高度不该被下一行盖住；与 ImGui 自动排版不要混用同一区域 |
| `ui_theme.h` | 样式 token（颜色按语义、度量按用途） | `apply()` 无 ImGui 上下文时是 no-op；换主题 = `GUi_ThemeSetPreset` + `GUi_ThemeApply` |
| `ui_input.h` | 输入门控与快捷键表 | 修饰键精确匹配；注册期报冲突，不等用户按下去才发现 |
| `ui_snapshot.h` | 面板视图模型（可序列化纯数据） | 64 位 id 在 JSON 里走**十进制字符串**（`JSON.parse` 只有 53 位精度）；读取侧宽容：缺字段用默认、坏行跳过 |
| `ui_layout_tree.h` | 布局树（保留模式，原名 `ui_component.h`） | 定位规则唯一判据是"父级是不是布局节点"；id 单调递增永不复用；`resolve()` 幂等 |

对应 C 面已在 `ui_api.h` 打开：`GUi_MakeId` / `GUi_State*` / `GUi_Layout*` / `GUi_Theme*` / `GUi_ShouldDispatchShortcut` / `GUi_Shortcut*`。用法模式：

```c
uint64_t id = GUi_MakeId("Opacity", entity_uuid);   // 跨帧同一个 id
float v = 0.5f;
GUi_StateGetFloat(id, &v);                           // 取（没有则用默认值）
if (GUi_SliderFloat("Opacity", &v, 0, 1, "%.2f")) {  // 画
    GUi_StateSetFloat(id, v);                        // 写回（引擎保管，不是栈上缓冲）
}
```

`GryceUiSmoke.exe`（`test/ui_smoke.cpp`）是这套东西的端到端演示，不进门禁、需要窗口：

```bash
build/GryceUiSmoke.exe                    # 暗色主题，画一块演示面板
build/GryceUiSmoke.exe --theme light      # 同一 token 表的亮色变体
build/GryceUiSmoke.exe --ops-burst 200    # 灌 200 条操作：统计里 coalesced≈99、drained=101
build/GryceUiSmoke.exe --ui-interval 3    # 节流截图与每帧构建逐字节一致（md5 相同）
```

`--ops-burst` 灌的是一半离散（`key=0`）+ 一半连续（`key=1`）的操作：连续项在队列里合并成一条（只落地最后的值），离散项按每帧预算分批落地——这正是"UI 低刷新率、操作受控执行"的完整闭环。

## 7.7 布局树（保留模式）

`GUi_Layout*` 是**即时模式**的矩形栈（相同调用序列 → 相同矩形）；布局树是**保留模式**的另一半：建一次树，之后只改参数，每帧 `GUi_CompResolve()` 重算矩形。适合"界面结构稳定、内容随状态变"的面板。

> **命名**：这棵树的类型叫 `gryce_ui::UiLayoutNode` / `UiLayoutTree`（原 `Component` / `ComponentTree`）。改名是因为引擎的 ECS 已有 `gryce_engine::Component`，而 P0/P2 之后两者的父子语义完全不同——ECS 那条轴是**类型继承 + 实例挂载**（`engine/components/component.h`、`architecture.md` 3.1/3.2），这里只是"矩形怎么排"。C 面 `GUi_Comp*` 与 `GUI_COMP_KIND_*` 名字不动。

```c
GUiCompSpec root{};                       // parent=0 = 无父级
root.kind = GUI_COMP_KIND_COLUMN;
root.x = 460; root.y = 24; root.w = 320; root.h = 480;   // 根必须写 pos/size
root.gap = 6;
for (int i = 0; i < 4; ++i) root.padding[i] = 8;         // 上 右 下 左
uint64_t col = GUi_CompAdd(&root);        // 返回值即节点 id（恒非 0）

GUiCompSpec row{};
row.parent = col; row.kind = GUI_COMP_KIND_ROW;
row.align = GUI_COMP_ALIGN_STRETCH;       // 交叉轴吃满
uint64_t head = GUi_CompAdd(&row);        // x/y/w/h 由布局填，传 0 即可

GUiCompSpec leaf{};
leaf.parent = head; leaf.kind = GUI_COMP_KIND_LEAF;
leaf.flex = 1.0f;                         // 主轴剩余空间按 flex 权重分
GUi_CompAdd(&leaf);

GUi_CompResolve();                        // 幂等：每帧调一次
GRUiRect r;
GUi_CompRect(leaf, &r);                   // 取解析结果
```

**定位规则只有一条：父级是不是布局节点。**

* 父级是 `Column` / `Row` / `Grid` → 子节点的 `x/y/w/h` 由布局安排（调用方写的值被忽略，子项传 0 即可）。
* 父级是 `Leaf` 或无父级 → 子节点**直接保留调用方写的 pos**，布局一个字节都不碰。所以"把一个节点从叶子换成 Column"会立刻翻转它所有子节点的定位方式——这是特性，不是 bug。

其余语义：

| 主题 | 规则 |
| --- | --- |
| 容器自身大小 | 布局**只摆位，不决定容器自身大小**；容器自己的 `w/h` 就是它的外框 |
| 内容区 | 容器内容区 = 自身 rect 向内缩 `padding`；flex 分配的是内容区里的主轴剩余空间 |
| flex | 只分配主轴剩余空间；剩余为负（放不下）时钳到 0，不产生负尺寸 |
| align | 交叉轴对齐：`Start` / `Center` / `End` / `Stretch`（吃满内容区，扣掉交叉 margin） |
| `Column` / `Row` | 主轴顺序堆叠，`gap` 是主轴间距 |
| `Grid` | 等宽列（`cols`），行高取行内请求高最大值，子节点填满格子 |
| 解析顺序 | `resolve_node` 先安排自己的子节点（写回 rect）再递归——父布局的写回一定发生在"以新 rect 摆布孙节点"之前 |
| id | 单调递增、**永不复用**；移除子树只是释放槽位。`add`/`set` 忽略调用方传入的 id 字段，返回值才是真 id |
| `set` 换父级 | 做环检查（把自己挂到自己的子孙名下会被拒绝）；父级未变时不动子列表位置 |
| `remove` | 连带整棵子树；实现上**先收集再统一释放**——逐点释放会在已释放节点上写 `next_sib`，而空闲链表正串在这个字段上 |

C 面见 `ui_api.h` 的「布局树（保留模式）」一节：`GUi_CompAdd/Set/Remove/Clear/Resolve/Rect/Get/Kind/Parent/ChildCount/ChildAt/Count`，容量 512，越界枚举值直接拒绝。

**每个节点画成一个窗口时的两条铁律**（`GryceUiSmoke` 的 Layout 页就是这么画的，见 `test/ui_smoke.cpp`）：

1. **不要传 `GUI_WIN_NO_BRING_TO_FRONT`。** 这个标志在 imgui 里会让新窗口创建时进显示序**队首**（画在最底层）——子窗口比父窗口创建得晚，反而被压到父窗口下面，不透明的窗口背景把整棵子树盖得只剩一个根。层级序就是创建序：子窗口创建在后，天然画在父窗口之上，这正是要的效果。
2. **节点尺寸不要小于 32px。** imgui 的 `WindowMinSize` 默认 (32, 32)，`SetNextWindowSize` 显式给小尺寸也会被钳制。容器若要在自己的第一行画标签，用 `padding` 上边预留一行（演示里是 24），别指望窗口自己缩下去。

演示：`--page layout`（或 `Ctrl+3`）画一棵 20 个节点的树——根 Column、flex 1:2 的标题行、交叉轴四种对齐的中间行、3×2 网格、脚注，外加一个"父级是叶子"的对照组（它的两个孩子直写 pos，布局不碰）。页内滑杆改根的 gap 与网格列数，走的是完整的"Get → 改 → Set → Resolve"保留模式闭环。

## 7.8 主题预设与美化

token 表分两部分：**17 个锚色**（每套预设必须给出的值）与**40 个派生色**（由锚色按固定规则算出，预设不直接给值）。派生规则集中在 `ui_theme.cpp` 的 `derive_extended()`：

| 派生 token | 规则 |
| --- | --- |
| `CHECK_MARK` / `SLIDER_GRAB` / `NAV_HIGHLIGHT` | = `ACCENT` |
| `SLIDER_GRAB_ACTIVE` / `RESIZE_GRIP_ACTIVE` / `SEPARATOR_ACTIVE` | = `ACCENT_ACTIVE` / `ACCENT` |
| `SEPARATOR` / `TABLE_BORDER_STRONG` | = `BORDER` |
| `TITLE_BG` / `MENUBAR_BG` / `TABLE_HEADER_BG` | = `HEADER` |
| `POPUP_BG` | = `WINDOW_BG` |
| `SCROLLBAR_GRAB_HOVERED` / `ACTIVE` | = `BUTTON_HOVER` / `BUTTON_ACTIVE` |
| 其余（滚动条底、grip、tab、plot、table 斑马纹、nav 变暗 …） | 锚色之间按固定比例的 `mix` / `shade`（提亮压暗） |

这样做的好处：**新增一套主题只要给 17 个值**，且"换预设后哪些 token 必须相等"可以直接写单测（`derived_tokens_consistent_across_presets` 逐套预设断言 `CHECK_MARK == ACCENT` 等式）。哪天某套预设开始手工抄派生值，这条用例就会红——那是"换预设漂移半个界面"的前兆。

预设清单（`GUI_THEME_PRESET_*`，编号与 `ThemePreset` 逐一对齐、静态断言钉死）：

| 编号 | 名称 | 定位 |
| --- | --- | --- |
| 0 | `Dark` | 默认深色（与 ImGui 自带深色接近，观感无跳变） |
| 1 | `Light` | 浅色 |
| 2 | `Midnight` | 深蓝夜航 |
| 3 | `Nord` | 北极青灰 |
| 4 | `HighContrast` | 纯黑高对比（投影/户外） |

美化度量（所有预设共用，几何不分主题）：`WindowRounding=6`、`ChildRounding=4`、`FrameRounding=4`、`GrabRounding=3`、`PopupRounding=4`、`TabRounding=4`、`ScrollbarSize=12`、`GrabMinSize=8`，加上原有的内边距/间距/缩进/边框。`apply()` 把这些一次性推成 ImGui `Style`（无 ImGui 上下文时是 no-op）。

```c
int n = GUi_ThemePresetCount();                 // 5
char name[24];
GUi_ThemePresetName(GUI_THEME_PRESET_NORD, name, sizeof(name));   // "Nord"
GUi_ThemeSetPreset(GUI_THEME_PRESET_MIDNIGHT);
GUi_ThemeApply();                               // 帧外调；每次换预设后一次
```

**个别覆盖不跨预设存活**：`set_preset` 整表重填（含派生 token），之前在预设上做的 `GUi_ThemeSetColor` 调色会被重置——这是故意的，否则"换预设"永远换不干净。

演示：Controls 页顶部的 `Theme preset` 组合框（换预设走操作队列，与"UI 只入队、宿主锁外执行"同一条规矩），或启动参数 `--theme dark|light|midnight|nord|highcontrast`。

## 7.9 动画系统（两块）

动画系统刻意分成两块，边界是"**要不要跨帧状态**"：

* **块一（纯函数层）**：`GUi_AnimEase` / `GUi_AnimTween` / `GUi_AnimColorLerp` / `GUi_AnimEaseName`。给定时间就算出值，不持有任何状态。"改一个属性并让它平滑过渡"只需要它——按钮 hover 抬起、面板滑入、数值滚动，都是"知道 elapsed 就能算出一个属性值"。
* **块二（运行时）**：`GUiAnimClip`（轨道表）+ 播放器（`GUi_AnimPlay/Pause/Stop/Resume/Seek/Advance/SetSpeed`）+ 具名通道读取。一个界面同时跑好几段交互动画（错峰入场、颜色循环、脉冲）时才需要它。

两块的边界不是画个境界线：运行时内部全部由块一的纯函数实现（`evaluate()` 就是反复调用 `anim_ease` / `AnimTrack::eval`），不重复造第二套数学。

### 块一：纯函数

```c
// 缓动：11 种，f(0)=0、f(1)=1 恒成立；Back/Elastic 在区间中段会超出 [0,1]
float e = GUi_AnimEase(GUI_ANIM_EASE_OUT_BACK, 0.7f);

// 补间：from -> to，duration 秒、delay 秒延迟；t 是补间开始后经过的秒数
float v;
GUi_AnimTween(0.0f, 1.0f, 1.2f, 0.0f, GUI_ANIM_EASE_IN_OUT_CUBIC, t, &v);
GUi_ProgressBar(v, -1, 0, nullptr);        // 直接当属性用

// 颜色插值
float col[4];
GUi_AnimColorLerp(red, blue, v, col);
GUi_ColorButton("swatch", col);
```

补间的边界语义：`t < delay` → `from`（延迟期间不动）；`t >= delay + duration` → **精确落到 `to`**（不受缓动形状影响）；`duration <= 0` → `to`（零时长就是瞬间到位）。循环不归纯函数管——时间折叠（`fmod`）是调用方的事，演示里就是这么做的。

### 块二：片段 / 轨道 / 通道 / 播放器

```c
GUiAnimClip clip{};                        // 容量：8 轨道 / 每轨 16 关键帧
clip.loop = GUI_ANIM_LOOP_LOOP;
clip.duration = 2.0f;                      // >0 = 显式时长；0 = 由轨道推出

GUiAnimTrack t{};                          // 标量轨道
strncpy(t.channel, "slide.x", sizeof(t.channel) - 1);
t.key_count = 3;
t.keys[0].t = 0.0f; t.keys[0].v = 0.0f;   t.keys[0].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
t.keys[1].t = 1.0f; t.keys[1].v = 200.0f; t.keys[1].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
t.keys[2].t = 2.0f; t.keys[2].v = 0.0f;   t.keys[2].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
clip.tracks[clip.track_count++] = t;

GUiAnimTrack c{};                          // 颜色轨道：端点插值
c.is_color = 1; c.duration = 1.6f; c.ease = GUI_ANIM_EASE_IN_OUT_QUAD;
c.color_from[0] = 1; c.color_to[2] = 1;
clip.tracks[clip.track_count++] = c;

GUi_AnimPlay(&clip, 1);                    // 播放（内部拷贝一份片段）
GUi_AnimAdvance(dt);                       // 帧循环里推进；时间源归调用方
float x;
GUi_AnimChannelFloat("slide.x", &x);       // 读通道；返回 1 = 有该标量通道
```

语义表：

| 主题 | 规则 |
| --- | --- |
| 通道 | 轨道把值写进**具名通道**；标量与颜色通道类型分明，读错类型返回 0（`channel_float` 读颜色通道、`channel_color` 读标量通道都是 0） |
| 段缓动 | 标量轨道上，第 i 个关键帧的 `ease` 作用于 `[keys[i], keys[i+1])` 区间（"从这一帧到下一帧用什么曲线"） |
| 区间外 | 首帧之前保持首帧值、尾帧之后保持尾帧值，不插到区间外 |
| 轨道 delay | 局部时间 = 片段时间 − delay；为负时保持初始值（标量 = 首帧值，颜色 = `color_from`）——错峰入场就靠它 |
| 循环 | `Once`（播完置 finished）、`Loop`（`fmod` 回绕）、`PingPong`（三角波折叠）；`GUi_AnimTime()` 报**未折叠**的累计时间 |
| 片段时长 | 显式 `duration` 优先，否则取所有轨道 `(delay + 时长)` 最大值；短于它的轨道在尾部保持终值 |
| `play` | 内部拷贝片段（调用方的片段之后可以随便改）；`restart=0` 保留当前时间（改循环模式/参数时用） |
| 通道表 | 每次 `play` 以本片段的轨道为准重建——换片段后读不到上一段的残留值 |
| 播放控制 | `stop` 停播但**通道保留最后的值**（画面定格）；`pause`/`resume`；`resume` 在播完之后会从头再来；`set_speed` 钳到 [0, 64] |
| 线程 | 纯数据、无锁；`advance` 在帧循环锁外调，回调内读通道与 `GUi_CompRect` 同级 |

**与布局树的配合**：动画不认识节点，节点也不依赖动画。宿主把通道值写进节点 rect（`GUi_CompGet` → 改 → `GUi_CompSet`）。注意这些节点的父级应该设为 0（无父级）——否则父布局会在 `resolve()` 时覆盖动画写进去的 pos。反过来说，7.7 的"父级不是布局节点就保留直写 pos"这条规则，正好被动画系统复用：**动画直写 pos，布局不碰**。

演示：`--page anim`（或 `Ctrl+4`）。面板里是块一（缓动组合框 + 补间进度条 + 颜色插值色块，时间用宿主时钟）；右侧舞台是块二——一根 2 秒的片段驱动四个节点：`slide`（滑移，InOutCubic 关键帧）、`pulse`（两次脉冲）、`hop`（两段不等高弹跳）、`stag.0..3`（delay 0/0.15/0.30/0.45 错峰入场）。页内可改循环模式（不重置进度）、速度、拖动 seek，通道列表实时显示每个通道的值。

## 7.10 内置 UI 渲染路径（脱离 ImGui）与 UIDrawList

`GUi_*` 目前经 Dear ImGui 落地。引擎在做**内置 UI 运行时**：retained 控件树（`ui_layout_tree`）+ 自绘渲染，最终不再依赖 ImGui。分四阶段推进，每阶段独立可验证：

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | `ui_drawlist` 命令缓冲（模型层，纯逻辑） | 完成 |
| 2 | 渲染桥：drawlist → `IRenderer2D`，三后端自动继承，不写新 shader | 完成（7.11） |
| 3 | retained 控件层：命中测试 + 事件分发 + 控件库（Button/Label/Slider/Checkbox/TextInput） | 完成（7.11） |
| 4 | `GUi_*` 双轨切换，ImGui 退居可选调试后端 | 完成（7.11） |

ImGui 在这条路径上的定位随之变化：**从"UI 系统本体"退为"编辑器调试 UI 的一个后端"**。UI 模型层（7.6）与布局树（7.7）、动画（7.9）都不含 ImGui 依赖，换后端不伤它们。

**为什么渲染桥不需要新 shader**：光栅化载体是引擎已有的 `IRenderer2D`（`engine/render/render2d.h`）——`draw_rect` / `draw_text` / `draw_sprite_region` / `set_scissor` / `top_left_origin`（该接口的注释里就写着"UI/HUD 层"）三后端（D3D12 / Vulkan / GL）均已实现；文本用引擎自己的 `FontAtlas`（stb_truetype、多字体源 Latin+CJK 回退、glyph UV/advance）。内置 UI 只补"缺的那部分"：命令缓冲与控件语义。

### UIDrawList 语义表（`engine/ui/ui_drawlist.h`）

| 主题 | 规则 |
| --- | --- |
| 定位 | 模型层：只记录"画什么"（命令 + 裁剪栈 + 字符串池），不碰 GPU，可无窗口单测 |
| 命令 | `Rect` / `Text` / `Sprite` / `PushClip` / `PopClip` / `RoundRect` / `RoundRectBorder` / `Shadow` 八类；POD，渲染桥与测试共用 |
| 圆角族字段 | `radii[4]`（左上/右上/右下/左下）、`border_w`（描边线宽 / 阴影扩散半径）、`color2`（非 0 = 竖直渐变的下端色）；半径单位是**逻辑像素**，跟着 UI 缩放一起放大 |
| 坐标系 | 左上原点、+y 向下，与 `IRenderer2D` 的 `top_left_origin` 一致 |
| 裁剪相交 | `push_clip` 与当前裁剪取交集；交集为空（w≤0 或 h≤0）进入剔除态 |
| 剔除态 | 后续**绘制命令**直接丢弃并计 `culled()`；`PushClip`/`PopClip` 照常记录（否则恢复不回来） |
| 命令裁剪标记 | 每条命令带 `clip` 字段 = **执行该命令后**生效的裁剪栈深度（PushClip 记压入后、PopClip 记弹出后、绘制命令记当前深度；0 = 无裁剪）；渲染桥据此切 scissor，不在命令里存裁剪矩形 |
| 裁剪栈满 | 拒绝 push 并计 `dropped()`；`pop` 只按深度弹，未成功的 push 不会让后续 pop 失衡 |
| 命令表满 | 丢弃并计 `dropped()`（容量 4096） |
| 字符串池 | 容量 32KB；放不下整条文本时**整条丢弃并计数，不截断**——半条字符串比没有更难调试 |
| 空文本 | `nullptr` 或空串不产生命令（不计 culled） |
| 几何 | 不规范化：w/h 为负原样记录，几何正确性由布局层负责 |

演示宿主验证方式：`GryceUiSmoke --ui-backend builtin --page controls|queue|layout|anim|struct|style`——不经过 ImGui、直接由内置即时模式层生成 drawlist 并经渲染桥画到屏幕，用于截图回归（与 `--ui-backend imgui` 同一份宿主代码，只换后端）。`struct` 页演示折叠头/树/可滚子区域/弹层/模态/提示；`style` 页把圆角/渐变/描边/投影一次摆出来（含带标题栏的浮窗）；`--ui-scale N` 验证 HiDPI。

## 7.11 内置 UI 轨（`GRender_SetUIBackend(1)`）

四阶段全部落地后的形态：`GUi_*` 有两套实现，**同一份宿主代码**两边都能跑。

```c
GRender_SetUIBackend(1);      // 必须在 GRender_Init 之前：决定建不建 ImGui 上下文
GRender_SetUICallback(cb, user);   // 回调签名不变，两条轨一样
GRender_Init(&desc);
```

| 主题 | 规则 |
| --- | --- |
| 后端选择 | 进程级、init 前生效。0 = ImGui（默认，行为与旧版完全一致）；1 = 内置 |
| 谁画 | ImGui 轨：ImGui 的 draw data → 命令；内置轨：`UiDrawList` → 2D 覆盖层 pass |
| 回调位置 | 相同：`GRender_EndFrame` 内、`g_renderer.mutex` 已持有 |
| 节流 | 相同：`GRender_SetUiFrameInterval` 对两条轨都是"每 N 帧才重建"，非构建帧重绘上次结果 |
| 上下文 | 内置轨**不建** ImGui 上下文（`GRender_HasImGui()` 为 false 是正常状态） |

### 一帧的时序（`engine/api/ui_builtin_backend.h`）

```
debug_ui_submit_locked()            主线程，锁已持有
  ├─ 拉输入（GInput_IsMouseButton*/GInput_PullChars/退格键）→ UiInputFrame
  ├─ ui.begin_frame(...)            注入主题 + 文本度量
  ├─ 宿主回调                       只提交控件/只入队
  ├─ ui.end_frame() + ui.draw()     收帧、回收未提交控件、产出 UiDrawList
  └─ renderer2d->begin_overlay_pass / 桥 / end_overlay_pass
```

**为什么在同一处同步画、而不是像 ImGui 那样压成渲染命令**：`Renderer2D` 的成员（相机、顶点批次、屏幕尺寸）只由主线程写，渲染线程只执行它压进去的命令。把覆盖层 pass 打包成命令交给渲染线程执行，会与下一帧主线程的写入竞争——症状是随机的堆损坏（`0xC0000374`），不是稳定崩溃。覆盖层 pass 本身也只记录顶点、再把顶点压成命令，所以 GL 调用仍然排在场景命令之后、UI 盖在最终画面上。

### 内置轨的能力边界

内置轨与 ImGui 轨的**控件集合已经对齐**：同一份宿主代码两条轨都能跑完整流程。下面只列差异与语义细节。

| 主题 | 内置轨行为 |
| --- | --- |
| 外观 | 控件与窗口 chrome 全部走圆角 + 纵向渐变 + 描边（半径档位与实现见 7.12）；浮窗、弹层、提示额外带投影。`Widget::corner_radius = 0` 可对单个控件关掉圆角（模态遮罩必须这么做） |
| `BeginWindow` / `EndWindow` | 支持。**标题栏默认就有**（`GUI_WIN_NO_TITLE_BAR` 关掉，与 ImGui 的 `NoTitleBar` 同向），可拖动、左侧折叠框（`GUI_WIN_NO_COLLAPSE` 去掉）；菜单栏要显式给 `GUI_WIN_MENU_BAR`，不给就完全不画。`GUI_WIN_NO_MOVE` / `NO_COLLAPSE` / `NO_SCROLLBAR` / `NO_RESIZE` 生效；位移/折叠/滚动按窗口 id 跨帧存活（宿主每帧重给矩形也不丢）。标题栏上两角圆、下两角直，底边压 1px BORDER 当分隔线 |
| `Text` / `TextColored` / `Separator` / `Spacing` / `SameLine` / `LabelText` / `Indent` / `Unindent` | 支持；`Text` 按内容区宽度**自动折行** |
| `Button` / `ButtonSize` / `SmallButton` / `Checkbox` / `SliderFloat` / `InputText` / `ColorButton` / `ProgressBar` | 支持（`SmallButton` = 普通按钮；`ColorButton` 只画色块，标签只当 id）。悬停/按下换描边色而非换底色，渐变幅度 2%~8% 不压文字 |
| `DragFloat` | 支持真拖拽：按住后按**鼠标位移**改值（`speed` 单位/像素，按住 Ctrl 走 1/10 精调）；没有"点哪算哪"的语义 |
| `DragFloat2/3/4` | 支持：一行里并排 N 个只显示数值的框（等宽，按 `PushMultiItemsWidths` 的口径切分整行），标签文本跟在最后一个框右边。N 个框是 N 个独立控件（id 由「标签 + 分量序号」派生），拖哪个改哪个；任一分量变化即返回 1 |
| `Selectable` | 支持：整行宽、左对齐文本、选中态高亮。`selected` 是**入参状态**（选中集合归调用方管），返回值只表示"被点了"，不自行改 `selected`——与 `ImGui::Selectable(label, bool)` 一致 |
| `Image` | 支持：走 drawlist 的 sprite 通道交给 `Renderer2D` 采样，`tex` 是**打包的引擎纹理句柄**（见 5 节「纹理 id 的两套语义」）。与 ImGui 的差别只在激活/导航：ImGui 那边图片没有 id（不进导航、不进 `ActiveId`），内置轨给图片一个由纹理句柄派生的 id，于是它能被按下（点视口图会顺带清掉键盘焦点，这正是想要的行为）。**悬停两条轨都报得出来**——ImGui 的 `IsItemHovered` 是 `LastItemData` 的纯矩形判定，与 id 无关——所以 `LastItemRect` + `IsItemHovered` 的拾取链两边一致 |
| `ColorEdit4` | 色块 + 点开的弹层（R/G/B/A 四条滑块），弹层内改动即时回写 |
| `Combo` | 真下拉：点开弹层 + 菜单项，选中后回写并关闭 |
| `BeginChild` / `EndChild` | 支持：自有裁剪域（超出不可见也不可点）、自有滚轮与滚动偏移。`size=(0,0)` = 占满剩余空间（与 ImGui 同义）。`border = true` 画圆角描边（裁剪仍按矩形，四角不会切进内容） |
| `TreeNode` / `TreePop` / `CollapsingHeader` | 支持：展开态跨帧存活，子项自动缩进 |
| `BeginMenuBar` / `BeginMenu` / `EndMenu` / `MenuItem` | 支持：菜单就是"贴着按钮下沿打开的弹层"，点菜单项自动关闭 |
| 弹层：`OpenPopup` / `OpenPopupOnItemClick` / `BeginPopup` / `EndPopup` / `CloseCurrentPopup` / `IsPopupOpen` | 支持。弹层画在第二层（永远盖住窗口），自适应高度；点外面或 Esc 关闭（同一帧 `BeginPopup` 即返回 0，与 ImGui 的 NewFrame 语义一致） |
| 模态：`OpenModal` / `BeginModal` | 支持：居中、铺一层暗底遮罩、**点外面不关**，Esc 或 `CloseCurrentPopup` 才关 |
| 提示：`SetTooltip` | 支持：贴在鼠标位置，`time > 0` 时延迟 0.35 秒出现（单测里 `time = 0` 立即出现，便于截图复现） |
| 拖放：`BeginDragDropSource` / `SetDragDropPayloadString` / `EndDragDropSource` / `BeginDragDropTarget` / `AcceptDragDropPayloadString` | 支持：拖出 4px 阈值才成立，payload = (类型串, 文本)，松手在目标上时交付 |
| 查询：`LastItemRect` / `IsItemHovered` / `IsItemClicked` / `IsItemActive` / `IsAnyItemActive` / `MousePos` / `IsMouse*` / `GetModifiers` | 支持 |
| `MouseDeltaX/Y` / `SetCursorPos` | 恒 0 / 空操作（要 delta 请直接读 `GInput_*`） |
| `WantCaptureMouse` | 鼠标落在任何一个面板/弹层矩形内即为真 |
| `WantCaptureKeyboard` | 输入框拿到焦点时为真 |

### 文本编辑（`InputText` 在内置轨的真实语义）

| 输入 | 行为 |
| --- | --- |
| 点击 / 拖选 | 插入点按**文本真实宽度**换算到字节偏移（始终落在 UTF-8 码点边界上）；拖动=拖选 |
| ← → / Home / End | 逐码点移动 / 行首行尾；带 Shift 时拉选区 |
| Ctrl+← → | 按词移动（`text_word_left/right`） |
| Backspace / Delete | 有选区删选区，无选区删前/后一个码点 |
| Ctrl+A / C / X / V | 全选 / 复制 / 剪切 / 粘贴，走进程级剪贴板 `ui_clipboard()`（宿主可直接读写，便于镜像到系统剪贴板） |
| Esc | 清选区并失焦 |
| 输入法 | 预编辑串（`UiInputFrame::preedit`）以**带下划线**的形式画在插入点处，只属于当帧持有焦点的输入框；组合中不写回调用方缓冲区 |
| 插入符闪烁 | 由 `UiInputFrame::time` 驱动（1.06 秒一周期，亮 0.53 秒）；`time == 0` 恒亮，单测与截图因此可复现 |
| 长文本 | 横向滚动保证插入点可见，文本不淌出框外 |

### 文本与坐标的四个前提

1. **度量走同一个口径**：自动尺寸（按钮宽 = 文字宽 + 内边距）与文字居中都用 `IRenderer2D::measure_text`（字形 advance 之和，UTF-8 按码点累加）；渲染器不支持度量时退回 `ui_approx_text_width` 估算。两边必须是同一份，否则"布局按估算、绘制按真实"会互相漂移。
2. **`draw_text` 按码点取字形**：2D 文本路径已改成 UTF-8 解码（`Renderer2D::draw_text`），CJK 因此能出字；图集里没有的码点跳过。
3. **HiDPI 走 `GRender_SetUIScale`**：模型层永远按**逻辑像素**算（布局、命中、文本度量都是逻辑值），输入坐标在 `begin_frame` 里除以倍率，渲染桥把矩形、字号、裁剪框乘回去。宿主只需在 init 后设一次倍率，不要自己缩放面板坐标。
4. **字号要落到整像素**：逻辑字号 × 倍率之后由 `ui_text_physical_size()` 取整，**度量与绘制都必须过这个函数**（宽度按 19.5 算、字形按 20 画，长串文字能差出一个字）。取整的真正理由是字形图集按整数像素分档烘（下一节），绘制尺寸与某一档相等才是 1:1 采样。

### 文字质量：抗锯齿 + 字形尺寸阶梯

三件事一起决定"字看着清不清楚"，缺一件都会糊：

| 环节 | 做法 | 不做会怎样 |
| --- | --- | --- |
| 覆盖度 | stb_truetype 逐像素算出的 8bit 覆盖度**原样当 alpha**，片元着色器不做阈值化 | 原来写过 `smoothstep(0.45, 0.55, alpha)`，等于把覆盖度压成 0/1，字缘全是硬阶梯——视觉上就是"没有抗锯齿" |
| 栅格尺寸 | 图集里同一批字形按**整数像素分档**烘（`FontAtlas` 的尺寸阶梯：`ensure_size` / `bucket_for`），绘制时选相对误差最小的一档 | 单张 32px 图集画 13px 界面文字＝缩到 0.4 倍，笔画两侧各糊一圈灰边（实测比 ImGui 的 1:1 明显软）；反过来放大则是马赛克 |
| 像素对齐 | 缩比≈1 时把每个字形原点 `round` 到整像素（`Renderer2D::draw_text` 里 `snap`） | 字落在半像素上会被双线性采样平均成两列半灰，锐度掉一半 |

分档策略（`Renderer2D::ensure_text_raster_size`）：

* 启动时按 `ui_text_physical_size(13, ui_scale)` 烘一档（1 倍屏是 13、1.5 倍是 20、2 倍是 26——**这就是"支持 DPI"的落点：2 倍屏不是把 13px 的图放大两倍，而是重新烘了一份 26px 的字**）。
* 再固定烘一档 32px 通用尺寸，供引擎自带叠加层（FPS 文字 22px）与 2D Label 这类任意字号使用，免得它们被 13px 放大 1.7 倍。
* 档位有 12% 容差、上限 6 档；加档＝整张图集重烘 + 重传纹理，所以只在 `GRender_SetUIScale` 真的改了倍率时发生（运行期重建走 `RenderContext::run_with_gpu_context_sync`，宿主别在 UI 回调里调）。
* 宿主可以完全不碰这些：不调 `GRender_SetUIScale` 就是 1 倍屏、13px 一档，字照样清晰。

### 渲染侧为本轨新增的接口（`engine/render/render2d.h`）

| 接口 | 用途 |
| --- | --- |
| `measure_text(text, size)` | 文本宽度（返回 0 = 无度量，调用方退回估算） |
| `ensure_text_raster_size(px)` / `text_raster_size()` | 字形图集按物理字号重烘（DPI/缩放）。默认空实现，不支持的后端退化成缩放绘制 |
| `begin_overlay_pass(w,h)` / `end_overlay_pass()` | UI 覆盖层独立成帧：存/还相机、切左上角 1:1、关 bloom 与阴影 |
| `set_scissor` / `reset_scissor` 先 `flush_batches()` | 裁剪是状态、绘制是顶点：改裁剪前必须把已缓冲顶点提交，否则前后两段一起吃最后一次裁剪域 |
| `GRender_SetUIScale(f)` / `GRender_GetUIScale()` | HiDPI 倍率（顺带把字形图集补到对应的物理字号档）。GL 后端在 `set_scissor` 里按需开 `GL_SCISSOR_TEST`，换渲染目标时关掉并作废缓存（否则离屏 pass 会被上一段的屏幕坐标裁剪域切掉） |
| `GInput_GetWheel(dx,dy)` / `GInput_InjectPreedit(s)` / `GInput_GetPreedit(buf,cap)` | 滚轮（取走即清零）与输入法组合串 |

## 7.12 圆角 / 渐变 / 描边 / 投影（美化）

内置轨的外观全部由**主题 token + 渲染器图元**两层决定：颜色/度量在 `ui_theme.h`，几何光栅化在 `IRenderer2D`。中间不引入任何新的着色器——圆角是 CPU 侧展开的折线多边形。

| 主题 | 规则 |
| --- | --- |
| 六档圆角 | `WINDOW_ROUNDING=8`（窗口）、`POPUP_ROUNDING=8`（弹层/提示）、`CHILD_ROUNDING=8`（子区域）、`FRAME_ROUNDING=4`（按钮/输入框/进度条）、`GRAB_ROUNDING=4`（复选框/菜单高亮）、`TAB_ROUNDING=4`（页签）。Fluent 的"大面板圆、小控件方"层次。全都只用几何、不随预设变 |
| 单控件覆盖 | `Widget::corner_radius`：`< 0` 用上面那档默认值，`0` = 明确要直角（模态遮罩铺满屏幕时必须给 0，否则四角漏出没压暗的底图） |
| 投影开关 | `Widget::shadow`：只有浮窗（带标题栏）、弹层、工具提示打开。平铺面板互相贴着，影子画进邻居身下等于白花顶点。强度/扩散由主题 `style.shadow_alpha` / `shadow_spread` 决定（Fluent 0.5/26px 环境光，Flex+Blender 0.3/10px 最小离地感；`shadow_alpha=0` 时连紧影也不画） |
| 渐变 | 竖直渐变的两端色由 `shade()` 从同一个 token 推出来，不额外增加 token。**幅度是主题数据**（`style.fill_grad` / `panel_grad`，见 7.13）：Fluent 预设归零（纯平），Flex+Blender 预设给 12% 的 Halo 光泽 |
| 描边 | 线宽向内生长：外轮廓尺寸不因加边框变大，相邻控件的对齐不会因为有没有边框而漂 1 像素。**控件、面板、子区域统一用 `BORDER` token**（ImGui 的 `RenderFrameBorder` 全局只有 `ImGuiCol_Border` 一条，两轨要对齐就得同源）；悬停/聚焦才换 `ACCENT`。渲染器一侧的描边带严格贴框内缘 `[0, thickness]`——早先外沿放在「内缩 0.5」、内沿放在「内缩 1.5」，1px 边整条骑在半像素上，光栅化的 top-left 规则把上/右判成 2px、下/左判成 1px，同一圈边框四个方向不等粗 |
| 聚焦态 | 输入框聚焦时描边换成 `ACCENT` 并加粗 0.5 像素；悬停的拖拽框/复选框描边也换 `ACCENT` |

渲染器一侧（`engine/render/render2d.h`，默认实现退化成直角，所以第三方 `IRenderer2D` 实现不会因为 UI 用圆角而编不过）：

```c
draw_round_rect(x, y, w, h, radii, color);                 // 填充
draw_round_rect_gradient(x, y, w, h, radii, top, bottom);  // 竖直渐变
draw_round_rect_border(x, y, w, h, radii, thickness, c);   // 描边（向内）
draw_round_shadow(x, y, w, h, radii, spread, c);           // 外阴影
```

抗锯齿不用 SDF/新 shader：外轮廓按角展开成折线（每角 `arc_segments()` 段），填充用中心扇形，边缘再加一条**1 像素宽、alpha 由实到 0 的环带**——顶点色插值本身就把硬边化成软边。阴影是 8 层同心环 + `(1-t)²` 衰减，看上去接近高斯模糊，但只花顶点、不需要离屏目标。

两个容易踩的点：

* **内轮廓与外轮廓的顶点数必须相等**，否则环带直接被丢（症状：描边/阴影只剩外半圈、内侧留一道空白）。所以分段数由**基准半径**算一次，不随外扩量变；半径为 0 的角也照常输出 `seg+1` 个重合点（法线沿两边插值），点数恒为 `4*(seg+1)`。
* **半径要钳到 `min(w,h)/2`**：`22px` 的行高配 `6px` 圆角没问题，但胶囊形抓柄是 `半径 = 半宽`，不钳的话窄控件会画出翻折的自交多边形。

覆盖到的控件：按钮、面板/浮窗、进度条（框底+填充）、复选框（方框 + 三点折线勾）、滑块（框即轨道 + 胶囊抓柄）、拖拽数值框、输入框、组合框（框 + 右侧三角）、颜色编辑（4 个带通道色标的分量框 + 色块）、标题栏（只有上两角圆）、折叠头、菜单项高亮、滚动条（轨道+滑块）、工具提示、子区域边框。直角命令仍保留：菜单栏底条、分隔线、选区高亮、插入符。

## 7.13 JSON 主题与交互动画

主题可以整套放在 JSON 文件里加载，不必写 C 代码、不必重编译：

```c
GUi_ThemeLoadFromFile("assets/themes/flex-blender-dark.json");   // 1/0
GUi_ThemeLoadFromJson(json_text);                                // 内存中的主题文本
```

文件格式（五块全可选，缺项由出厂默认与派生规则补齐）：

```json
{
  "name": "Flex Blender Dark",
  "anchors": { "window_bg": "#2B2B2B", "accent": "#ED9E5C", "...": "..." },
  "colors":  { "popup_bg": "#232323", "control_border": "#6E6E6E" },
  "metrics": { "window_rounding": 5, "frame_rounding": 3 },
  "style":   { "fill_grad": 0.12, "panel_grad": 0.05,
               "shadow_alpha": 0.3, "shadow_spread": 10 },
  "animation": { "enabled": true, "hover_ms": 70, "press_ms": 50, "ease": "out_cubic" }
}
```

加载语义：以出厂深色预设的 17 个锚色为底 → 套 `anchors` → `derive_extended()` 派生 → 套 `colors` 覆盖派生结果 → 套 `metrics`/`style`/`animation`。所以只写 `"anchors": {"window_bg": ...}` 的主题文件也是良定义的。颜色写 `"#RRGGBB"` / `"#RRGGBBAA"`（`#` 可省略）；token 名是枚举的 snake_case（`window_bg`、`control_border`、`frame_rounding` 等，全表见 `engine/ui/ui_theme.cpp` 的名字表）。类型不对的项跳过、非法 JSON 返回 0 且不动当前主题。

`style` 块把「怎么画」也变成主题数据（`ThemeStyle`，`engine/ui/ui_theme.h`）：

| 字段 | 含义 | 出厂默认 | Fluent | Flex+Blender |
| --- | --- | --- | --- | --- |
| `fill_grad` | 控件填充的纵向渐变幅度（上端提亮比例；0 = 纯平） | 0 | 0 | 0.12 |
| `panel_grad` | 面板/窗口/标题栏的渐变幅度（比 `fill_grad` 收敛一档） | 0 | 0 | 0.05 |
| `shadow_alpha` | 浮窗/弹层/提示的投影强度（0 = 不画） | 0 | 0.5 | 0.3 |
| `shadow_spread` | 投影扩散半径（逻辑像素） | 0 | 26 | 10 |

**出厂风格是扁平的**（四个字段全 0）：内置轨与 ImGui 轨的默认观感必须一致，而 ImGui 那边是纯色面板 + 1px 描边 + 无影。质感是主题数据——想要渐变/投影就在 JSON 的 `style` 块里显式写，引擎不预设。

Fluent 与 Flex+Blender 因此是**同一套绘制代码的两个数据点**，不是两个绘制分支：Fluent 靠状态色（悬停/按下的亮度差）分层，光泽与投影全部关掉；Flex/Blender 靠 12% 的顶部提亮渐变做「Halo 光泽按钮」，投影只留最小的一点离地关系。渐变幅度钳制在 [0, 0.5]、投影 alpha 钳制在 [0, 1]，负值（会把画成凹坑）与越界值都被夹回合法域。

随引擎交付四套皮肤（`assets/themes/`）：**fluent-dark / fluent-light**（Win11 Fluent 正宗档：Mica 近似中性底、8px 面板 + 4px 控件、无渐变、大而柔的环境光投影）与 **flex-blender-dark / flex-blender-light**（Adobe Flex Halo × Blender：灰底、亮边描边、5/3px 小圆角、Halo 光泽渐变；深色用 Blender 橙 #ED9E5C 与选中蓝 #4772B3，浅色用 Flex 经典蓝 #2E6BD6）。

`animation` 块驱动内置轨的**状态过渡动画**：参与控件（按钮、拖拽框、复选框、滑杆抓柄、菜单高亮、折叠头）的主填充色在悬停/按下/勾选切换时按 `hover_ms`/`press_ms` 与缓动曲线插值；菜单高亮额外带淡入淡出。动画由 `BuiltinUi::draw` 用输入快照的 `time` 差分推进——`time == 0`（单测/截图）时一切瞬变，画面可复现。retained 宿主不调用 `advance_animations()` 即得到瞬变行为。`set_preset` 不重置动画参数；`animation` 块缺省时保持现状。

## 7.14 两轨一致性（内置轨 ↔ ImGui 轨）

`GRender_SetUIBackend(0)`（ImGui）与 `(1)`（内置轨）跑同一份页面代码时，**观感必须一致**：同一套主题 token、同一套几何规则、同一个字体。下面这些常数和对齐口径不要就地改——改一处，两轨就在那一处开始漂。

| 项 | 规则 |
| --- | --- |
| 窗口 chrome | 标题栏**默认有**（`GUI_WIN_NO_TITLE_BAR` 关）；菜单栏要显式 `GUI_WIN_MENU_BAR`。标题栏高 = 帧高 = `字号 + 2×(PAD_Y−1)` = **22**，底边 1px `BORDER` 当分隔线，其下 5px 窗口底色 |
| 行高 | 控件行高 = **字号本身**（14），不按字体度量的 1.25 倍；帧高 22 = 14 + 2×4 |
| 项宽 | `item_width() = trunc(child_size.x × 0.65)`（ImGui 的 `ItemWidthDefault`，注意用的是子窗口**整宽**，别扣边框）。复选框/滑块/拖拽框/输入框/组合框/颜色编辑都按它排；框右边 `4px` 处接标签 |
| 框 | 填充 `BUTTON`（本主题里就是 ImGui 的 `FrameBg`）+ 1px `BORDER` 内描边；圆角 `FRAME_ROUNDING` |
| 滑块 | **框即轨道**（不是「细轨道 + 填充条」）：框内数值居中、抓柄 8px 宽圆角竖条、位置在「左右各缩 2px + 半个抓柄」的区间里按比例插值 |
| 复选框 | 方框边长 = 框高（22）；选中底 `checkbox_bg`、勾是三点折线（线宽 = 边长/5）；标签在框右 `+4px` |
| 组合框 | 框内左侧当前项（内边距 8px，超出裁剪）+ 右侧 `RenderArrow(Down)` 实心三角；标签在框右 |
| 颜色编辑 | 分量宽 = `floor((项宽 − (框高 + 4) − 3×4) / 4)`；每个分量框左缘 2px 通道色标（红/绿/蓝/白），色块 = 框高见方 |
| 描边 | 一切框统一 `BORDER`，1px 严格贴内缘（见 7.12） |
| 字体 | `kWidgetFontSize = 14`，两轨同款 **Cousine**：ImGui 轨内嵌的 `kCousineRegularTtf` 就是它，内置轨经 Renderer2D 图集加载。查找顺序 `res:/fonts/Cousine-Regular.ttf` → `res:/fonts/Roboto-Medium.ttf`（老项目沿用）→ `third_party/imgui/misc/fonts/` 下的同两款 → 系统字体。**字体不同就谈不上对齐**：等宽/比例的差异会让每个词的实际宽度都变，换行位置随之漂移 |

已知残余差异（**布局、配色、几何无差**，剩下的是采样方式）：

* **字形抗锯齿**：ImGui 用 stb_truetype 的覆盖度 + 自己的 alpha 通道插值，内置轨走 Renderer2D 的字形图集；同一字形同一字号下笔画位置一致，但边缘灰阶分布不同——按像素比对时每个字都有一圈"毛边"差异，肉眼看不出。
* **圆角弧的抗锯齿**：弧是折线多边形 + 1px alpha 环带，`radius=8` 时轮廓近似到 1.1px 阶梯；ImGui 的 `AddRect` 用 0.5px 半宽描边 + 抗锯齿填充，边缘更软。

`GryceUiSmoke --page controls --ui-backend builtin|imgui --shot x.png` 可以随时采两轨同页截图做像素比对（`build/pngdiff.py`）。

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
* 拖放 payload 只支持单个字符串，没有多选拖拽、没有自定义 payload 类型（跨进程/跨窗口拖拽也不支持）。
* 窗口只能拖动/折叠，没有可拉的边框（`GUI_WIN_NO_RESIZE` 恒生效：尺寸由宿主的布局决定）。
* 字体图集按字节走 ASCII 与 CJK 回退，没有富文本/多字号混排；一个控件只有一种字号。
* 主题内置 5 套预设（Dark / Light / Midnight / Nord / HighContrast，见 7.8）；调用方只引用 token，不要在调用方硬编码色值。
* `GUi_Image` 不接受自定义 UV / 边框着色，视口只显示完整纹理；`w` / `h` 必须都 > 0（与 ImGui 的 `Image` 一致，只收显式尺寸）。
* `GUi_Image` 的 `tex` 有两套语义，按 UI 轨区分（见 5 节「纹理 id 的两套语义」）：内置轨传打包的引擎纹理句柄，不是 `ImTextureID`。
* 中文等非 ASCII 文本依赖引擎加载的字体图集；字体缺失时显示为方块，不会崩。
