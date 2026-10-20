# Gryce Editor —— 项目本体

这是编辑器作为"一个 Gryce 项目"的形态：宿主 `GryceApp.exe` 只负责初始化引擎、加载本目录的
`main.js`，其余全部由脚本与扩展决定。

本项目**不含任何 C++ 源码**。所有 C++ 都在 `../ext/`（功能拓展），两者只通过
`engine.editor.*` 这层 JS 接口对话。

## 目录

| 文件 | 作用 |
| --- | --- |
| `main.js` | 引导脚本：建窗、开离屏视口、视口相机导航、鼠标 → Gizmo/拾取、快捷键分发 |
| `selftest.js` | 接口自检：50 条断言，覆盖扩展加载、面板数据、命令、撤销、拾取 |
| `project.data` | 项目配置：窗口 1600x900、OpenGL、关掉 SSAO/SSR/PCSS（视口要稳定） |
| `scenes/main.gesc` | 启动场景：相机 / 平行光 / 立方体 / 地面 / Props（含两个子物体） |
| `models/cube.obj` | 程序生成的单位立方体，不依赖仓库其它资源 |
| `extension.dll` | 构建产物，由 `GryceEditorExtension` 自动拷入；**不入库** |

## 运行

```bash
# 引擎 + 扩展
cmake --build build --target GryceApp GryceEditorExtension

# 正常启动
build/GryceApp.exe --project editor/project

# 无窗口跑帧（CI / 冒烟）
build/GryceApp.exe --project editor/project --frames 60

# 接口自检：最后一行为 SELFTEST PASS 50/50
build/GryceApp.exe --project editor/project --entry res:/selftest.js --frames 45
```

扩展没编出来时不会崩：`engine.editor` 是 `undefined`，`main.js` 退化成自由相机浏览器，
日志里给一条 warn。这条降级路径要一直保留 —— 否则扩展一编译失败，编辑器连场景都打不开。

## 操作（当前形态）

| 输入 | 行为 |
| --- | --- |
| 左键点击物体 | 拾取并选中 |
| 左键拖拽 Gizmo 轴 | 平移 / 旋转 / 缩放（整段记一步撤销） |
| 右键拖拽 | 转视角 |
| 右键 / 中键按住 + WASD | 平移视口相机 |
| W / E / R | 切换 Gizmo 工具 |
| F | 聚焦选中实体 |
| G | 线框切换（仅 OpenGL 桌面版支持） |
| Ctrl+Z / Ctrl+Shift+Z | 撤销 / 重做 |
| Ctrl+S | 保存 |
| Delete | 删除选择集 |
| Space | 播放 / 停止 |

## 说明

- **窗口是空的**。视口输出打开后画面写进离屏纹理，窗口只留给 UI；UI 层还没接上，
  所以看到的是清屏色。这不是渲染失败，`selftest` 里 `view_info.output=true` 就是证据。
- 场景是 JSON（`.gesc` v2），可以直接手改，也可以由编辑器保存。
- 着色器不随项目走：`res:/shaders` 缺失时引擎回退到内置目录。
- 打包：`GryceGC package --build-dir build --project . --name "Gryce Editor" --verify`。
