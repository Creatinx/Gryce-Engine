# Gryce Engine — 命令行参数参考

> 本文档描述 `gryce-engine.exe`（编辑器/运行器）以及示例程序支持的命令行参数。所有路径参数均支持 `res:/` 虚拟路径与相对路径。

---

## 通用参数

| 参数 | 说明 | 示例 |
|---|---|---|
| `--help`, `-h` | 打印帮助信息并退出 | `--help` |
| `--project-root <path>` | 指定游戏项目根目录（即 `res:/` 指向的实际目录）。默认使用当前工作目录。 | `--project-root examples/3dtest` |
| `--vulkan` | 使用 Vulkan 渲染后端。默认使用 OpenGL。 | `--vulkan` |
| `--scene <scene_name>` | 启动时加载指定场景文件（`.gesc`）。路径相对于项目根目录，可带或不带 `.gesc` 后缀。 | `--scene scenes/main` |
| `--resolution <WxH>` | 设置窗口分辨率。默认 `1920x1080`。 | `--resolution 1280x720` |
| `--headless` | 无窗口/离屏渲染模式。截图与录屏时无需显示窗口。 | `--headless` |
| `--auto-close <seconds>` | 启动 N 秒后自动退出。常用于自动化测试与 CI。 | `--auto-close 3` |
| `--no-audio` | 禁用音频系统。 | `--no-audio` |

---

## 截图参数

| 参数 | 说明 | 示例 |
|---|---|---|
| `--screenshot <path>` | 启动后截取一帧并保存为 PNG。 | `--screenshot cli_test.png` |

- 截图分辨率固定为窗口分辨率（默认 1920×1080）。
- 配合 `--headless` 可在无窗口环境下输出 PNG。
- 输出路径支持相对路径；若使用 `res:/` 路径则解析到项目根目录。

---

## 录制参数

| 参数 | 说明 | 示例 |
|---|---|---|
| `--record <seconds>` | 录制 N 秒视频。 | `--record 5` |
| `--output <path>` | 录制/截图的输出路径。 | `--output clips/pbr.mp4` |
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

## 典型调用示例

```bash
# 1. 加载 PBR 展示场景，录制 5 秒轨道相机视频
gryce-engine.exe --scene demos/pbr_showcase.gesc --record 5 \
  --output clips/pbr.mp4 --camera orbit --headless

# 2. 加载 2D 演示，录制 3 秒内置相机动画
gryce-engine.exe --scene demos/gt2d_demo.gesc --record 3 \
  --output clips/2d_jump.mp4 --camera demo --headless

# 3. 编辑器无窗口截图
gryce-engine.exe --scene editor.gesc --screenshot clips/editor_ui.png \
  --resolution 1920x1080 --headless --auto-close 2

# 4. 仅打印帮助
gryce-engine.exe --help
```

---

## 实现位置

- 参数解析入口：`editor/editor_app.cpp` / 各示例 `main.cpp`。
- 相机预设控制器：`editor/camera_presets.cpp`（如存在）或示例内置逻辑。
- 截图/录制后端：`core/render/opengl/gl_backend.cpp`、`core/render/vulkan/vk_backend.cpp`。
