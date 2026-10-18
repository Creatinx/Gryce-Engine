# 着色器规范

着色器是四后端（OpenGL / Vulkan / D3D12 / WebGL）共用的源码，因此有多条必须遵守的硬约束。违反时的症状通常是"某个 pass 静默失效、画面变空或花屏"，且不伴随明显报错。

## 1. 源码组织与查找

- GLSL 源统一位于 `engine/render/shaders/`；运行期查找逻辑在 `render/core_shaders.cpp`。
- 变体优先规则在 `render/shader_source_resolver.cpp`：优先 `vulkan_{name}` / `webgl_{name}`，缺失回退基名。
- 演示工程的预编译 SPIR-V（`test/demo_project/shaders/`）只是兜底产物；`*.spv.hash` 为本地缓存指纹，不入库。

## 2. 宏定义：同一个宏全仓只允许一处定义

D3D12 的 vulkanize（`d3d12_vulkanize`）会把源码里**所有** `#define NAME 值` 重放到文件开头，且不区分 `#if` 分支。因此下面这种两分支写法会让同名宏出现两个替换值：

```glsl
// 禁止：vulkanize 重放后 shaderc 报 Macro redefined，整个 stage 失败
#ifdef GRYCE_HAS_PCSS
#define GRYCE_HAS_PCSS 0
#else
#define GRYCE_HAS_PCSS 1
#endif
```

按后端变化的开关一律写成"一处 `#ifndef` 兜底"，0 值由后端前缀注入（例如 `gl_shader.cpp` 的 ESSL300 转换为 WebGL 注入 0）：

```glsl
#ifndef GRYCE_HAS_PCSS
#define GRYCE_HAS_PCSS 1
#endif
```

## 3. 新增着色器的配套要求

- 新增 shader 必须补 `vulkan_*` 变体（Vulkan 优先分支找不到会回退基名，但 SPIR-V 与 GLSL 语义差异需要显式变体）。
- D3D12 只用 GL 版源码，经 vulkanize 翻译；缺变体会在 first-run 编译失败，表现为整个 pass 静默失效。
- WebGL 下采样器计数是编译期硬约束，新增采样器前先读第 5 节。

## 4. ESSL300 转换约束（WebGL）

`gl_shader.cpp` 的 `to_glsl_es300()` 在编译期做四项机械转换，对着色器源码的写法有对应要求：

| 约束 | 原因 |
| --- | --- |
| `#version` 必须在第一行 | ESSL 与 ANGLE 都要求版本行位于首行，连前置注释都会报错 |
| fragment shader 需要精度声明 | ES 300 没有 float/int 默认精度，转换器会注入 `precision highp ...` 块 |
| 不允许 uniform 初始化器 | ESSL 3.00 禁止（ANGLE 报 `cannot initialize this type of qualifier`），桌面 GLSL 330 允许；引擎所有 uniform 由 CPU 侧每帧写入，剥离不改变行为。项目自定义 shader 若依赖 shader 侧默认值，会在这里显形 |
| `texture2D(` 需兼容 `texture(` | 转换器做防御性替换 |

## 5. 纹理单元预算

- 桌面 GL 给 32 个片段纹理单元；**真实 GPU 的 WebGL2 只给 16**（SwiftShader 上报 32，所以软件光栅下永不暴露、真机必黑）。
- GLSL 的采样器计数发生在编译期：声明并被引用就算占用一个单元，运行期 `if` 省不掉。
- pbr 系着色器按后端裁特性：`pbr.frag` / `instanced.frag` 25 → 14、`skinned_pbr.frag` 18 → 14（裁 PCSS 原始深度、VSM/ESM、SDFGI、点光阴影）。
- 槽位到单元的映射按 `max_units` 分档（`render/opengl/gl_slot_map.h`）：16 单元走紧凑表，裁掉的采样器绑到 16+ 死区只绑定不采样；同一单元不能混用 `sampler2D` 与 `samplerCube`。

## 6. 深度解析口径

`depth_resolve` 必须使用 `depthTest=VK_TRUE` + `COMPARE_OP_ALWAYS` + `depthWrite=VK_TRUE`：始终通过测试并写深度，只做解析不做筛选。

## 7. 阴影偏置

`slope_bias()` = `max(base*(1-N·L), base*0.3) * 2.0`。接收端共七份着色器必须同步：`pbr` / `instanced` / `skinned_pbr` / `vulkan_*` 变体 / `deferred_lighting`。不同步时临界自遮挡会随 PCF 的逐像素旋转角翻成 4px 周期斜条纹。
