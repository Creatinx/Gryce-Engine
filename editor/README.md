# editor —— Gryce 编辑器

本目录是 Gryce 编辑器的**宿主工程**。它不修改引擎，只消费引擎已经公开的 C ABI；
引擎缺什么，先判断该不该进引擎，再决定落点（见第 2 节）。

分两块，互不依赖源码：

```
editor/
├── project/        一块完整的 Gryce 项目（编辑器本体，能直接 GryceApp 跑起来）
└── ext/            一块功能拓展（编辑器能力的声明与实现，编成 extension.dll）
```

`project/` 不 include 任何 C++ 头，只写 `main.js` 与 `project.data`；`ext/` 不碰
`project/` 的任何文件，只通过 JS 命名空间 `engine.editor.*` 对接。两者的唯一契约
是 `ext/ext.h` 里的声明表。

---

## 1. 两块各自是什么

### project/ —— 完整项目

`GryceGC new` 生成的骨架加编辑器引导逻辑，形态与普通项目完全一致：

| 文件 | 作用 |
| --- | --- |
| `project.data` | 窗口、后端、渲染开关（编辑器默认 OpenGL + 关掉 SSAO/SSR/PCSS，视口要稳定不抖）；`"start_in_play": false` 让宿主停在编辑态 |
| `main.js` | 引导脚本：建窗、开离屏视口、驱动相机与快捷键、每帧调 `engine.editor.tick()` |
| `scenes/main.gesc` | 编辑器启动场景（相机 + 平行光 + 地面 + 一个参考立方体） |
| `models/cube.obj` | 参考几何，程序生成，不依赖仓库其它资源 |
| `extension.dll` | 构建产物，`ext/` 编出来拷贝到这里；**不入库** |

`main.js` 里没有任何编辑逻辑——它只负责"读输入 → 叫 `engine.editor.*` → 收结果"。
把编辑规则写进 JS 会让同一套规则在两处存在（JS 一份、C++ 一份），这是本项目
刻意避免的。

### ext/ —— 功能拓展

编成 `extension.dll`，由宿主 `dlopen`，在 `main.js` 求值之前注册 `engine.editor.*`。
`ext.h` 是**声明**，`ext.cpp` 是装配，其余子目录是**实现**：

```
ext/
├── ext.h              ★ 声明层：编辑器需要什么能力，每条标注归属
├── ext.cpp            装配：注册入口 + 模块初始化 + tick 分发
├── bridge/            实现：JS 值通道（GJsValue ↔ C 类型）与注册
├── session/           实现：文档状态、选择集、脏标记
├── command/           实现：GEdit_* 封装、历史分组、待确认创建
├── view/              实现：视口状态、显示模式、Gizmo 交互状态机
├── asset/             实现：资产扫描、侧车读写、导入设置
├── panel/             实现：层级树 / 属性 / 资产 / 统计的面板数据模型
└── input/             实现：快捷键表与每帧轮询
```

---

## 2. 规则：能力该落在哪

ext 是**声明**，不是"什么东西都往这塞"。新增一项能力时按顺序问三个问题，
第一个答"是"的地方就是它的家：

### 规则一：引擎能做，就加在引擎

判据：**脱离编辑器也有意义**。打包、热更、资源校验、其它宿主、其它项目都可能
用到 → 进 `engine/api/`，编进 `GryceCore.dll` / `GryceRenderer.dll`。

这样做的好处是能力对所有人可见，不需要每个项目各写一份扩展；坏处是引擎接口
一旦公开就要维持兼容，所以门槛要严——"编辑器专属"不算理由。

本文档创建时，按这条规则补进了引擎的：

| 能力 | 落点 | 理由 |
| --- | --- | --- |
| `GAsset_ScanDirectory` | `engine/api/asset_api.h` | 资产浏览器要列目录，但打包清单、热更校验、CI 资源检查同样要；写完一次，所有人能用 |
| `GEdit_TranslateEntity` | `engine/api/edit_api.h` | 增量平移不是"编辑器想要"，而是 `GEdit_SetTransform` 的通用缺陷：它在**入队时**用当前值补齐缺省分量，同一帧"先 SetTransform 再累加位移"会互相覆盖。任何做拖拽 / 动画 / 程序化位移的宿主都会踩到 |

已经具备、无需再补的（此前几次迭代补齐的）：编辑命令面 `GEdit_*`、撤销栈
`GCore_Undo/Redo/BeginEdit`、视口纹理 `GRender_GetViewportTexture`、双视口
`GRender_RenderGameView`、Gizmo `GRender_*Gizmo*`、线框
`GRender_SetDisplayMode`、资产侧车 `GAsset_ReadMeta/WriteMeta`。

### 规则二：引擎能做但只对编辑器有意义，就在 ext 里包一层

判据：底层原语已经有了，缺的只是"编辑器语义"。

| 能力 | 引擎原语 | ext 补的语义 |
| --- | --- | --- |
| 多选 | 单选 `GEdit_SelectEntity` / `GEntity_GetSelected` | 选择集、主选中与附选的区分、多选删除 |
| 变换参数归一 | `GEdit_SetTransform`（绝对值）/ `GEdit_TranslateEntity`（增量） | 按轴过滤、Gizmo 拖拽的像素位移换算成世界增量 |
| 待确认创建 | `GEdit_CreateEntity`（入队，不回句柄） | 记 pending，下一帧用"创建即选中"约定确认句柄 |
| 编辑历史分组 | `GCore_BeginEdit/EndEdit` | 按操作类型自动配对，异常路径保证 EndEdit |
| 脏标记 | `GScene_GetCurrentPath`（无脏标记） | 用"最后保存时的场景 JSON 指纹"比对 |
| 相机导航 | `GViewport_SetEditorCamera` | 轨道/飞行两套模式的状态与速度曲线 |
| 快捷键 | `engine.input.key_down` | 声明式绑定表 + 每帧边沿检测 + 冲突检测 |

### 规则三：引擎不做，ext 自己实现

判据：纯编辑器概念，引擎没有必要知道。

- 面板数据模型（层级树、属性列表、资产分组）——引擎不该有"面板"这个概念
- 会话状态（当前文档、脏标记、最近打开）
- 快捷键映射与冲突检测
- 视口布局（Scene / Game 双视口谁大谁小）

### 一句话总结

> 通用能力进引擎，编辑器语义进 ext 声明，纯编辑器概念进 ext 实现。
> 凡是"改一行引擎就能让所有项目受益"的，不要在 ext 里用 C ABI 拼。

---

## 3. 归属判定表

声明层 `ext.h` 里每条能力都带 `@owner` 标注，取值与下表一致。新增能力必须
同时更新这里，否则视作未完成。

| 能力组 | 声明 | 归属 | 说明 |
| --- | --- | --- | --- |
| 会话 | `editor.session.*` | ext 实现 | 文档生命周期、脏标记 |
| 选择 | `editor.selection.*` | ext 实现 | 多选集；主选中同步到引擎单选 |
| 命令 | `editor.command.*` | ext 实现（薄封装） | 参数归一后转 `GEdit_*`，返回 1/0 |
| 历史 | `editor.history.*` | 转调引擎 | `GCore_Undo/Redo` + 分组策略在 ext |
| 视口 | `editor.view.*` | 转调引擎 | `GRender_*` / `GViewport_*` |
| Gizmo | `editor.gizmo.*` | 转调引擎 + ext 状态机 | 命中/绘制/换算都在引擎；手势状态与单帧夹取是 ext |
| 资产 | `editor.asset.*` | 转调引擎 | 目录列举走 `GAsset_ScanDirectory` |
| 面板 | `editor.panel.*` | ext 实现 | 产出 JSON 给 UI 层 |
| 快捷键 | `editor.shortcut.*` | ext 实现 | 声明式绑定 |

---

## 4. 跑起来

```bash
# 1. 引擎（首次或改了引擎代码）
cmake -S . -B build
cmake --build build --target GryceApp

# 2. 扩展（拷到项目目录，宿主默认加载 <project>/extension.dll）
cmake --build build --target GryceEditorExtension

# 3. 启动
build/GryceApp.exe --project editor/project

# 无窗口跑 N 帧做逻辑验证（CI 用这条）
build/GryceApp.exe --project editor/project --frames 60
```

扩展缺失时不会崩：`engine.editor` 是 `undefined`，`main.js` 走降级分支（只跑
相机导航），日志里给一条 warn。这条路径必须一直可用——否则扩展一编译失败，
编辑器就完全打不开，连日志都看不到。

**必须停在编辑态**：宿主默认进 Play（运行时语义），而 Play 态下引擎冻结撤销
栈（播放期的物理/动画改动不该进编辑历史）。所以 `project.data` 写了
`"start_in_play": false`；临时对比运行时行为可以加 `--edit-mode` 的相反用法
——不写这个字段或显式跑 Play，症状是"编辑都生效，但撤销永远灰、脏标记永远干净"。
启动时打印的 `[app] start mode: edit` 就是用来确认这一点的。

---

## 5. 加一项新能力的五步

1. **判归属**：按第 2 节的三个问题走一遍，决定进引擎还是 ext。
2. **写声明**：在 `ext.h` 加结构体/函数声明，带 `@owner` 与一行"为什么"。
3. **写实现**：进对应子目录，不在 `ext.cpp` 里堆逻辑（它是装配层）。
4. **接 JS**：在 `bridge/js_register.cpp` 挂到 `engine.editor.*`，参数用
   `bridge/js_args.h` 的读取函数，错误统一 `throw_error`。
5. **验证**：`GryceApp --project editor/project --frames 60` 能跑通，日志无
   error；涉及引擎改动的，同时补 `ctest` 用例。

---

## 6. 边界

- **ext 不画 UI**。它产出面板**数据**（JSON），画由宿主或后续 UI 层负责。
  这样 ext 不需要 include `imgui.h`，也不会被 ImGui 版本绑死。
- **ext 不注册帧回调**。所有"每帧"的工作由 `main.js` 显式调
  `engine.editor.tick(dt)` 驱动，顺序可控、可重入、CI 里能确定性地跑。
- **ext 不做线程**。引擎的 `GCore_BeginFrame`、`GRender_*` 都在主线程，扩展
  跟着主线程走，否则要自己处理跨线程队列。
- **ext 不 include 引擎内部 C++ 头**（`scene/entity.h` 之类），只 include
  `engine/api/*.h`。内部头会把扩展绑死在一次编译的 C++ ABI 上。
- **命令是异步的**。`GEdit_*` 只入队，下一帧 `GCore_BeginFrame` 才生效。凡是
  "改完立刻读"的接口都要接受这一点，见 `command/commands.h` 里 pending 的处
  理方式；同一帧内多条变换必须用增量版 `GEdit_TranslateEntity`，否则后一条会
  把前一条覆盖掉。

---

相关文档：[编辑器接口](../docs/zh/editor-api.md)、[原生扩展](../docs/zh/extension.md)、
[应用入口与脚本](../docs/zh/app-entry.md)。
