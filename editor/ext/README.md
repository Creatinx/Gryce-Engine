# ext —— 功能拓展

编辑器能力的**声明**与**实现**。编成 `extension.dll`，由宿主 `dlopen`，在 `main.js`
求值之前把 `engine.editor.*` 注册好。

规则本身在 [`../README.md`](../README.md) 第 2 节，这里只说模块分工。

## 模块

| 目录 | 职责 | 归属 |
| --- | --- | --- |
| `ext.h` | **声明层**，唯一事实源。每条能力带 `@owner` 标注 | — |
| `ext.cpp` | 装配：初始化、轮转缓冲、参数透传。不放判断逻辑 | — |
| `bridge/` | JS 值通道：`GJsValue` ↔ C 类型；`engine.editor.*` 注册表 | ext |
| `session/` | 文档状态、脏标记、多选集（引擎只有单选） | ext |
| `command/` | `GEdit_*` 封装、待确认创建、复合操作分组 | wrapper |
| `view/` | 视口状态与显示模式；Gizmo 手势与选中同步 | wrapper |
| `asset/` | 资产目录列举（转发引擎）、侧车信息、结果缓存 | wrapper |
| `panel/` | 层级树 / 检视 / 资产 / 统计的**数据模型**（不出界面） | ext |
| `input/` | 快捷键表：声明式绑定、冲突检测、每帧边沿轮询 | ext |
| `common/` | JSON 拼装（不引 nlohmann，扩展不做解析） | — |

`@owner` 三档：`engine` 表示这条已进引擎、ext 只转调；`wrapper` 表示引擎有原语、
ext 补编辑器语义；`ext` 表示纯编辑器概念。`ext.h` 里不出现需要新建引擎能力的声明 ——
那种需求先去改 `engine/api/`。

## 编译期约束

- 只 include `engine/api/*.h`（C ABI），**不** include 引擎内部 C++ 头。
- 只链接 `GryceCore` / `GryceRenderer` / `GrycePlatform` 的导入库。
- 不依赖 `imgui.h`：面板只产出 JSON，绘制是宿主的事。

## 已知限制

- **命令是异步的**。`GEdit_*` 入队后要等下一帧 `GCore_BeginFrame` 才执行。
  连续创建两个实体时只有最后一个能确认句柄（引擎"创建即选中"只保留最后一次）。
  `command/commands.cpp` 里用 pending 队列 + 4 帧过期兜底，事件里会给
  `create_unconfirmed`。
- **视口相机只写不读**。引擎的 `GViewport_SetEditorCamera` 没有对应的 Get，
  所以 `view/viewport.cpp` 自己存了一份副本用于 `screen_ray`（拾取必须用视口
  相机的朝向，不能用场景里被编辑的 Camera 实体）。
- **线框只有 OpenGL 桌面版**。`view_displayModeSupported("Wireframe")` 会如实
  返回，切换前应当先问。
