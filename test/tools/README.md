# test/tools — 渲染后端人工验证脚本

按项目约定，渲染后端不进 CTest（依赖 GPU / 驱动 / 无头环境不可靠），
验证方式是 `GryceRenderSmoke.exe --api <gl|vulkan|dx12> --shot x.png` + 人工比对。

这里的 Python 脚本把"肉眼比对"变成可复现的数值/文本结论（尤其在无法直接看图、
或在 CI/远程终端里排查时）：

| 脚本 | 用途 |
|---|---|
| `img_diff.py <参考图> <对照图> [--flip]` | 整体统计（均值、\|Δ\| 分布、唯一色数）+ 亮度 ASCII 缩略图 + 分块差异热力图。`--flip` 会先把对照图上下翻转再比，用来验证"整体是否上下镜像"。 |
| `diffregions.py <参考图> <对照图>` | 找出差异最大的连通区域，并对每个区域给出两图各自的均值 —— 用来定位"哪一块不一样"（黑网格、缺失的叠加层、错位的反射…）。 |
| `crop_ascii.py <图> x0 x1 y0 y1 [列 行]` | 裁剪一块区域打印 ASCII 缩略图（形状/边缘可用文本判读）。 |
| `crop_mean.py <图> x0 y0 x1 y1 [...]` | 打印若干矩形区域的均值，用于逐点取样。 |
| `palette.py <图> [...]` | 出现频率最高的颜色与低频色占比，用来判断"色阶丰富度"（缺后处理/缺 UI 文字会明显偏低）。 |
| `drop_entity.py <gesc> <实体名>` | 从场景 JSON 里删掉一个实体，做成最小复现工程（例如删掉粒子系统来判断某个挂起是否由它引起）。**别用字符串裁剪**，会把相邻实体截坏、留下半截 JSON 让场景静默加载失败。 |
| `cursor_lock_probe.ps1` | 端到端验证交互摄像机的指针策略：**仅窗口有焦点时锁定鼠标**、**按住 Alt 显示指针且不转视角**。它会拉起 `--play` 的冒烟窗口，用 Alt/Alt+Tab 制造状态变化，再按引擎自己打的 `cursor state: LOCKED/UNLOCKED (reason=…)` 日志校验状态机。脚本保持 ASCII（PowerShell 5.1 按 ANSI 读 .ps1，中文会变成解析错误）。 |
| `cursor_lock_stress.ps1` | 连按 N 次 Alt，统计"按下 ↔ 状态转换"是否 1:1（用来抓"指针锁定被别的代码每帧覆盖"这类问题）。 |
| `unique_colors.py` | 打印唯一颜色数：画面退化成平坦/量化（例如 SSR 整帧丢失）时会从 ~2900 掉到 ~590，是个与配置无关的快速检测器。 |

## D3D12 侧的环境变量开关（排查用，默认全关）

| 变量 | 作用 |
|---|---|
| `GRYCE_D3D12_SERIALIZE=1` | 每帧结束等 GPU 跑完（CPU 不再领先）——用于判断"竞态类"问题：开了就好 = 时序竞态。 |
| `GRYCE_D3D12_GPU_VALIDATION=1` | 打开 D3D12 **GPU 侧校验**（需先 `GRYCE_D3D12_DEBUG=1`）。专门抓"采样器模式不匹配""读到未同步写入"这类**普通调试层静默**的问题，常能把间歇性故障变成稳定复现。 |
| `GRYCE_TRACE_HDR=1` | 每帧打印 `[hdr]/[hdr2]/[hdr3]`：SSR 站点与 tonemap 实际拿到的是哪个纹理（判断"画面里的 SSR 丢失"发生在 CPU 侧还是 GPU 侧）。 |
| `GRYCE_DX12_TRACE_FRAME=<N>` | 打印第 N 帧里每次 pbr draw 的 CBV 地址与 uModel 平移（判断是否"某个物体拿到了别人的 uniform"）。 |
| `GRYCE_DX12_TRACE_BIND=1` | 打印 pbr 的反射相关采样器实际绑到的纹理（尺寸/格式/mip/是否 cube）。 |
| `GRYCE_D3D12_DUMP_HLSL=<dir>` | 把 SPIRV-Cross 生成的 HLSL 落盘。 |

典型用法（D3D12 对齐排查）：

```bash
GryceRenderSmoke.exe --api opengl --project test/demo_project --shot gl.png
GryceRenderSmoke.exe --api dx12   --project test/demo_project --shot dx.png
python test/tools/img_diff.py gl.png dx.png            # 先看整体差异与方向
python test/tools/diffregions.py gl.png dx.png         # 再看差异集中在哪
python test/tools/crop_ascii.py dx.png 700 1280 380 800
```

着色器链的中间产物也能落盘：

```bash
GRYCE_D3D12_DUMP_HLSL=hlsl_out GryceRenderSmoke.exe --api dx12 ...   # 看 SPIRV-Cross 生成的 HLSL
GRYCE_D3D12_DEBUG=1 GryceRenderSmoke.exe --api dx12 ...              # 打印 D3D12 调试层消息
```
