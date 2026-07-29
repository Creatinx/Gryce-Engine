# Gryce Engine — 命令行参数参考

> 本文档描述 `gryce-engine.exe`（编辑器/运行器）支持的命令行参数。所有路径参数均支持 `res:/` 虚拟路径与相对路径。

---

## 通用参数

| 参数 | 说明 | 示例 |
|---|---|---|
| `--help` | 打印帮助信息并退出 | `--help` |
| `--vulkan` | 使用 Vulkan 渲染后端（**默认后端**，该参数仅为兼容性保留）。 | `--vulkan` |
| `--opengl` | 使用 OpenGL 渲染后端（兼容后端，用于旧硬件/调试）。 | `--opengl` |
| `--vulkan-validation` | 启用 Vulkan 验证层（默认关闭）。 | `--vulkan-validation` |
| `--scene <scene_name>` | 启动时加载指定场景文件（`.gesc`）。路径相对于项目根目录，可带或不带 `.gesc` 后缀，也支持 `res:/` 路径。 | `--scene scenes/main` |
| `--resolution <WxH>` | 设置窗口分辨率。默认 `1920x1080`。 | `--resolution 1280x720` |
| `--headless` | 无窗口/离屏渲染模式（best-effort）。录屏时无需显示窗口。 | `--headless` |
| `--auto-close <seconds>` | 启动 N 秒后自动退出。常用于自动化测试与 CI；`--record` 时默认等于录制时长。 | `--auto-close 3` |
| `--no-audio` | 禁用音频系统（录屏时不采集系统音频）。 | `--no-audio` |

> **渲染后端分层**：Vulkan 为默认后端；OpenGL 为兼容后端；DirectX 11 / 12 是 `RenderAPI` 枚举中的预留值（WinNative），`create_render_backend` 对它们返回 `nullptr`，选择后会告警并回退到 Vulkan。未通过命令行指定后端时，使用项目设置（`project_settings.json` 的 `graphics.render_api`，可在编辑器 File > Project Settings 中修改）中的默认后端。

> **项目根目录**：不再有 `--project-root` 参数。项目根从可执行文件位置向上自动探测（优先 `editor/project/project.gryce`，其次 `examples/3dtest/project.gryce`，否则使用当前工作目录）；编辑器运行中可通过 **File > Load Project** 切换项目。

---

## 录制参数

| 参数 | 说明 | 示例 |
|---|---|---|
| `--record <seconds>` | 录制 N 秒视频，结束后自动退出。 | `--record 5` |
| `--output <path>` | 录制的输出路径。 | `--output clips/pbr.mp4` |
| `--camera <preset>` | 相机预设，见下表。 | `--camera orbit` |

### 相机预设

| 预设 | 说明 |
|---|---|
| `orbit` | 自动绕场景中心旋转展示。 |
| `flythrough` | 按预定义路径飞行。 |
| `static` | 固定最佳视角。 |
| `demo` | 播放场景内置的相机动画。 |

### 录制输出说明

- 当前实现首先写出 **PNG 序列**（每秒 30 帧，分辨率 1920×1080）。
- 如果系统 PATH 中存在 `ffmpeg`，会自动将 PNG 序列合成为 **MP4 (H.264, 30fps)** 并删除中间帧。
- 如果 `ffmpeg` 不可用，会保留 PNG 序列目录，并打印提示信息；用户可手动用 ffmpeg 合成：

```bash
ffmpeg -framerate 30 -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p output.mp4
```

- 录制时建议配合 `--headless` 与 `--no-audio` 使用，以避免窗口与系统音频干扰。

---

## CI / 自动化测试参数

以下参数不在 `--help` 中列出，供 CI 与冒烟测试使用（解析见 `editor/editor_app.cpp`）：

| 参数 | 说明 |
|---|---|
| `--test-play-mode` | 启动后自动进入并退出 Play Mode，用于验证场景快照/恢复逻辑。 |
| `--test-delete-undo` | 自动删除 Ground 实体再撤销，用于验证 Undo/Redo。 |
| `--auto-close <seconds>` | 见通用参数，与上述两者配合限定测试运行时长。 |

---

## 典型调用示例

```bash
# 1. 加载 PBR 展示场景，录制 5 秒轨道相机视频（默认 Vulkan 后端）
gryce-engine.exe --scene demos/pbr_showcase.gesc --record 5 \
  --output clips/pbr.mp4 --camera orbit --headless

# 2. 使用 OpenGL 兼容后端运行 2 秒冒烟
gryce-engine.exe --opengl --scene scenes/main --headless --auto-close 2

# 3. 编辑器无窗口冒烟运行
gryce-engine.exe --scene editor.gesc \
  --resolution 1920x1080 --headless --auto-close 2

# 4. CI 冒烟测试：自动进/出 Play Mode，3 秒后退出
gryce-engine.exe --test-play-mode --auto-close 3 --headless

# 5. 仅打印帮助
gryce-engine.exe --help
```

---

## 实现位置

- 参数解析：`editor/cli_args.cpp`（`parse_cli_args` / `print_help`）。
- 后端选择与 CI 参数（`--vulkan` / `--opengl` / `--vulkan-validation` / `--test-play-mode` / `--test-delete-undo` / `--auto-close`）：`editor/editor_app.cpp` 的 `EditorApp::run`。
- 相机预设控制器：`editor/camera_presets.cpp`。
- 截图/录制后端：`core/render/opengl/gl_backend.cpp`、`core/render/vulkan/vk_backend.cpp`。
