# 渲染器

本文描述渲染器的后端构成、管线结构与可调效果。构建选项见 [build.md](build.md)，着色器约束见 [shader-conventions.md](shader-conventions.md)，Web 端专项见 [webgl-backend.md](webgl-backend.md)。

## 1. 后端

四个后端共用上层管线与着色器源，经 `--api` 选择：

| 后端 | 值 | 加载方式 |
| --- | --- | --- |
| OpenGL | `opengl` | 链接期依赖 GLEW，运行期加载驱动 |
| Vulkan | `vulkan` | 运行时加载 `vulkan-1.dll` 与函数链，不绑定导入库；未装 Vulkan SDK 时后端关闭 |
| D3D12 | `dx12` | 运行时 `LoadLibrary` 加载 `d3d12.dll` / `dxgi.dll` / `dxcompiler.dll`，链接期不链导入库 |
| WebGL | `webgl` | Emscripten 构建专用，复用 GL 后端源码（见 [webgl-backend.md](webgl-backend.md)） |

后端经 `render/render.h` 的 `IRenderBackend` 接入。能力缺失按 `RenderBackendCapabilities` 声明式降级：compute shader 不可用时走 CPU 剔除（`--no-gpu-cull` 即回退入口），indirect draw 不可用时由 CPU 提交。

## 2. 前向管线

- Clustered Forward：光源按 3D 簇分配（`render/renderer_rd/cluster_builder_rd.cpp`）。
- HDR 渲染目标、色调映射、自动曝光。
- 法线预通道（`prepass_normal`）为 SSAO / SSR 重绘几何，跟随当前 LOD 级别。
- GPU 剔除（视锥 / 遮挡）与 CPU 回退路径并存。

## 3. 阴影

- Shadow Atlas + 级联阴影（CSM）。
- 滤波：VSM / ESM、PCSS 软阴影、点光源阴影、接触阴影。
- PCSS 在退化参数下自动退回 PCF 并打日志说明，判据与实测见 [performance.md](performance.md)。
- 阴影偏置口径（`slope_bias`）与七份接收端着色器的同步要求见 [shader-conventions.md](shader-conventions.md)。

## 4. 屏幕空间效果

SSR（反射）、SSAO、SSIL，可经 `--ssr` / `--no-ssr`、`--ssao` / `--no-ssao`、`--ssil` / `--no-ssil` 逐项开关。

硬件光追反射（Vulkan ray query，`GRender_SetRTReflection`）走独立通道，与 SSR 二选一；含逐像素追踪、时域降噪与 IBL 环境反射，详见 [rt-reflection.md](rt-reflection.md)。

## 5. 全局光照与环境

SDFGI、VoxelGI、Reflection Probe、IBL（预滤波环境贴图 + BRDF LUT）、体积雾。IBL 的镜面项口径（`F0*A + B`，禁用 `fresnel(NdotV)*A + B`）见 [model-import.md](model-import.md) 第 6.1 节。

## 6. 后处理

Bloom、TAA、运动模糊、Bokeh DOF、FSR2、次表面散射、水面。

## 7. 2D 管线

精灵、图块地图、视差背景、2D 粒子、2D 天空盒、文本。入口为 `render/render2d.cpp` 与 `render/renderer2d_impl.cpp`。

## 8. 3D 扩展

3D 粒子、线渲染、拖尾、实例化网格、Billboard、3D 文本、LOD（见 [lod.md](lod.md)）、体积光。骨骼网格由 `components/skinned_mesh_renderer.h` 驱动。

## 9. 调试工具

- 左上角叠加：FPS / 帧时间 / 限速状态（带帧时间曲线）。
- ImGui 调试窗口（F1 开关）：垂直同步、帧率上限、省电模式、GPU 计时，以及阴影 / GPU 剔除 / SSR / IBL 等渲染开关。
- 调试视图：`--debug-view N`（别名 `--debug`），N 为 0 最终合成、1 albedo、2 normal、3 roughness、4 metallic、5 阴影因子、6 直接光、7 环境光 / IBL、8 级联。
- `--list` 打印场景实体、相机、光源与网格统计。
- `--bench N` 分段 GPU 计时 + CPU 分相，量法与数据见 [performance.md](performance.md)。
