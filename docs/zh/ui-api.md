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
| 数值 | `GUi_Checkbox` / `GUi_SliderFloat` / `GUi_DragFloat` / `GUi_DragFloat2` / `GUi_DragFloat3` / `GUi_DragFloat4` / `GUi_ColorEdit4` |
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

### 纹理上下方向

`GUi_Image` 内部按后端自动决定要不要翻 V，调用方**不要**自己传 UV（接口也不提供）。原因是各后端的"纹理第一行"含义不同：

| 后端 | 渲染目标纹理第一行 | 是否翻 V |
|---|---|---|
| OpenGL | 图像的**下**边（GL 原点在左下） | 翻 |
| D3D12 / Vulkan | 图像的**上**边 | 不翻 |

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
* 主题只有一套暗色 + 语义文本色（`GUI_TEXT_*`）。要换主题改引擎内 `debug_imgui` 的 style，不要在调用方硬编码颜色。
* `GUi_Image` 不接受自定义 UV / 边框着色，视口只显示完整纹理。
* 中文等非 ASCII 文本依赖引擎加载的字体图集；字体缺失时显示为方块，不会崩。
