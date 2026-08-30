# 渲染器功能实现计划

> **For agentic workers:** 按任务逐项实现。每个任务产出独立可测试的功能。

**目标:** 将所有缺失的渲染功能集成到 RenderPipeline 主管线中

**架构:** 大部分功能已有 `renderer_rd/` 下的独立实现(SSR, DOF, Fog, Probe, GI, Decal)，
需要: (1) 创建 RenderPipeline 集成接口 (2) 创建主管线 shader (3) 插入 render_scene 流程。
少数功能(VSM/ESM, Shadow Atlas, Motion Blur, Water)需全新实现。

**技术栈:** C++, OpenGL/Vulkan, GLSL

---

### Task 1: P0-3 点光源双抛物面阴影 — 完成

**文件:**
- 修改: `core/render/render_pipeline.cpp` — 添加 create_point_shadow_targets, render_point_shadows, bind_point_shadow_uniforms
- 修改: `core/render/render_pipeline.h` — 已添加声明
- 修改: `examples/common/shaders/pbr.frag` — 点光源阴影采样函数已添加
- 新增: `examples/common/shaders/point_shadow_map.vert`
- 新增: `examples/common/shaders/point_shadow_map.frag`

- [x] **Step 1:** render_pipeline.h 已添加点光源阴影成员变量和方法声明
- [ ] **Step 2:** 实现 create_point_shadow_targets — 创建纹理和FBO
- [ ] **Step 3:** 实现 destroy_point_shadow_targets — 释放资源
- [ ] **Step 4:** 创建 point_shadow_map.vert — 双抛物面映射顶点着色器
- [ ] **Step 5:** 创建 point_shadow_map.frag — 深度写入
- [ ] **Step 6:** 实现 render_point_shadows — 遍历点光源渲染双抛物面
- [ ] **Step 7:** 实现 bind_point_shadow_uniforms — 上传uniform到主管线shader
- [ ] **Step 8:** 在 render_scene 中集成点光源阴影 pass
- [ ] **Step 9:** 在 render_deferred_lighting_to_hdr 中调用 bind_point_shadow_uniforms

### Task 2: P1-2 SSR 屏幕空间反射

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 SSR_RD 成员和集成接口
- 修改: `core/render/render_pipeline.cpp` — 集成 SSR 到渲染流程
- 新增: `examples/common/shaders/ssr.frag`
- 新增: `examples/common/shaders/ssr.vert`
- 新增: `examples/common/shaders/ssr_blur.frag`
- 新增: `examples/common/shaders/hiz_build.frag`

- [ ] **Step 1:** render_pipeline.h 添加 SSR_RD 成员和 set/get 接口
- [ ] **Step 2:** render_pipeline.cpp 在 init 中初始化 SSR_RD
- [ ] **Step 3:** render_pipeline.cpp 在 shutdown 中销毁 SSR_RD
- [ ] **Step 4:** 创建 ssr.vert/ssr.frag — 屏幕空间光线步进
- [ ] **Step 5:** 创建 ssr_blur.frag — SSR 双边模糊
- [ ] **Step 6:** 创建 hiz_build.frag — Hierarchical Z-Buffer 构建
- [ ] **Step 7:** 在 render_scene 中 Tonemap 前插入 SSR pass
- [ ] **Step 8:** 在 deferred_lighting.frag 中集成 SSR 采样

### Task 3: P1-3a 景深 (Bokeh DOF)

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 BokehDOF_RD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成 DOF
- 新增: `examples/common/shaders/dof_downsample.frag`
- 新增: `examples/common/shaders/dof_bokeh_h.frag`
- 新增: `examples/common/shaders/dof_bokeh_v.frag`
- 新增: `examples/common/shaders/dof_upsample.frag`

- [ ] **Step 1:** render_pipeline.h 添加 BokehDOF_RD 成员和 set/get
- [ ] **Step 2:** render_pipeline.cpp init/shutdown 中管理 DOF
- [ ] **Step 3:** 创建 DOF shaders (downsample, bokeh_h, bokeh_v, upsample)
- [ ] **Step 4:** 在 render_scene Bloom 后、Tonemap 前插入 DOF pass

### Task 4: P1-3b 运动模糊 (Motion Blur)

**文件:**
- 新增: `core/render/renderer_rd/effects/motion_blur.h`
- 新增: `core/render/renderer_rd/effects/motion_blur.cpp`
- 修改: `core/render/render_pipeline.h` — 添加 MotionBlur_RD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成 Motion Blur
- 新增: `examples/common/shaders/motion_blur.frag`
- 新增: `examples/common/shaders/motion_vectors.frag` (若不存在)

- [ ] **Step 1:** 创建 motion_blur.h — MotionBlur_RD 类声明
- [ ] **Step 2:** 创建 motion_blur.cpp — init, destroy, create_targets, render
- [ ] **Step 3:** 创建 motion_blur.frag — 基于速度缓冲的径向模糊
- [ ] **Step 4:** 创建 motion_vectors.frag (若 forward_clustered 已有则复用)
- [ ] **Step 5:** render_pipeline.h 集成 MotionBlur_RD
- [ ] **Step 6:** render_pipeline.cpp 在 DOF 后、Tonemap 前插入 Motion Blur

### Task 5: P1-4 体积雾 (Volumetric Fog)

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 VolumetricFog_RD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成体积雾
- 新增: `examples/common/shaders/fog.frag` (若 forward_clustered 已有则复用)
- 新增: `examples/common/shaders/fog_apply.frag`

- [ ] **Step 1:** render_pipeline.h 添加 VolumetricFog_RD 和 fog 参数接口
- [ ] **Step 2:** render_pipeline.cpp init/shutdown 管理 fog
- [ ] **Step 3:** 在 render_scene 中 PBR 后、后处理前插入 fog 渲染
- [ ] **Step 4:** 合成 fog 到 HDR 颜色

### Task 6: P1-5 Light Probe 系统

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 ReflectionProbeRD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成 Light Probe
- 修改: 编辑器中添加 probe 放置/管理 UI

- [ ] **Step 1:** render_pipeline.h 添加 ReflectionProbeRD 成员
- [ ] **Step 2:** render_pipeline.cpp init/shutdown 管理 probe
- [ ] **Step 3:** 在 render_scene 中更新最近 probe 绑定
- [ ] **Step 4:** 在 PBR/deferred shader 中集成 probe 采样

### Task 7: P2-1 VSM/ESM 可选阴影方案

**文件:**
- 新增: `core/render/renderer_rd/shadow/vsm_esm_shadow.h`
- 新增: `core/render/renderer_rd/shadow/vsm_esm_shadow.cpp`
- 修改: `core/render/render_pipeline.h` — 可选阴影模式
- 修改: `core/render/render_pipeline.cpp` — 集成 VSM/ESM
- 新增: `examples/common/shaders/shadow_vsm.frag`
- 新增: `examples/common/shaders/shadow_esm.frag`

- [ ] **Step 1:** 创建 VSM/ESM 阴影类
- [ ] **Step 2:** 实现 VSM 渲染：深度+深度² 输出，PCF 模糊
- [ ] **Step 3:** 实现 ESM 渲染：指数深度输出
- [ ] **Step 4:** 创建 VSM/ESM 采样 shader
- [ ] **Step 5:** render_pipeline 中集成可选阴影模式

### Task 8: P2-2 Shadow Map Atlas

**文件:**
- 新增: `core/render/renderer_rd/shadow/shadow_atlas.h`
- 新增: `core/render/renderer_rd/shadow/shadow_atlas.cpp`
- 修改: `core/render/render_pipeline.h` — 集成 Atlas
- 修改: `core/render/render_pipeline.cpp` — 使用 Atlas

- [ ] **Step 1:** 创建 ShadowAtlas 类 — 大纹理 + 动态分配
- [ ] **Step 2:** 实现分配/释放/碎片整理
- [ ] **Step 3:** 集成到 ShadowSystemRD
- [ ] **Step 4:** 修改 shader 支持 atlas 采样

### Task 9: P2-3 贴花系统 (Decal)

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 DecalStorage 成员
- 修改: `core/render/render_pipeline.cpp` — 集成贴花渲染
- 新增: `examples/common/shaders/decal.frag` (若 forward_clustered 已有则复用)
- 新增: `examples/common/shaders/decal.vert`

- [ ] **Step 1:** render_pipeline.h 集成 DecalStorage
- [ ] **Step 2:** render_pipeline.cpp 在 forward/GBuffer pass 中渲染贴花
- [ ] **Step 3:** 创建/复用 decal shader

### Task 10: P2-4 全局光照 (SDFGI + VoxelGI)

**文件:**
- 修改: `core/render/render_pipeline.h` — 添加 SDFGI_RD 和 VoxelGI_RD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成 GI
- 修改: `examples/common/shaders/pbr.frag` — 集成 GI 采样
- 修改: `examples/common/shaders/deferred_lighting.frag` — 集成 GI 采样

- [ ] **Step 1:** render_pipeline.h 集成 SDFGI_RD 和 VoxelGI_RD
- [ ] **Step 2:** render_pipeline.cpp init/shutdown 管理 GI
- [ ] **Step 3:** 在 render_scene 中更新 GI
- [ ] **Step 4:** 在 PBR/deferred shader 中采样 GI 间接光

### Task 11: P2-5 水渲染 (Water)

**文件:**
- 新增: `core/render/renderer_rd/effects/water.h`
- 新增: `core/render/renderer_rd/effects/water.cpp`
- 修改: `core/render/render_pipeline.h` — 添加 Water_RD 成员
- 修改: `core/render/render_pipeline.cpp` — 集成水渲染
- 新增: `examples/common/shaders/water.frag`
- 新增: `examples/common/shaders/water.vert`

- [ ] **Step 1:** 创建 Water_RD 类 — 水面网格、反射折射、波模拟
- [ ] **Step 2:** 实现水面渲染：顶点波 + 反射/折射 + 菲涅尔
- [ ] **Step 3:** render_pipeline 中集成 Water 渲染
- [ ] **Step 4:** 创建 water shader