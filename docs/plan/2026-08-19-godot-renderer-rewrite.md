# Godot 式渲染器重构实施计划

> **For agentic workers:** 本计划分 10 个阶段，按依赖顺序排列。每个阶段产生可独立工作的代码。

**目标：** 将 Gryce Engine 的渲染器重构为 Godot Forward Clustered 级别的渲染架构，包括 RendererCompositor 抽象、RenderingDevice 后端、Clustered 光剔除、Storage 系统、完整后处理链和实时 GI。

**架构：**
```
RenderingServer (API 层)
  └── RendererCompositor (渲染器抽象)
        ├── RendererCanvasRender (2D)
        └── RendererSceneRender (3D)
              └── RenderForwardClustered
                    ├── ClusterBuilder (光剔除)
                    ├── Depth Prepass + Motion Vectors
                    ├── Forward PBR + Sky
                    └── Post-Processing Chain
Storage 系统：
  ├── RendererMeshStorage / RendererMaterialStorage
  ├── RendererTextureStorage / RendererLightStorage
  └── RendererParticlesStorage / RendererUtilities
RenderingDevice (GPU 抽象) — 已有 Vulkan 后端
```

**技术栈：** C++17, Vulkan (已有), GLSL, SPIR-V

**当前已有基础：**
- `render/` — 已有 RenderPipeline（单一 Forward 管线）、OpenGL/Vulkan 后端、RHI 句柄
- `ecs/` — 已有 ECS 架构（World、ComponentStore、System）、HierarchySystem
- `scene/` — 已有 Entity（Node API）、Scene、Prefab、序列化

---

## 阶段 0：前置准备 — 清理现有渲染层

**前提：** 当前 RenderPipeline 是单一硬编码实现，需要先做接口抽象

**文件：**
- 新建: `core/render/renderer_compositor.h` — RendererCompositor 抽象基类
- 新建: `core/render/renderer_scene_render.h` — RendererSceneRender 抽象基类
- 新建: `core/render/renderer_canvas_render.h` — RendererCanvasRender 抽象基类
- 新建: `core/render/rendering_device.h` — RenderingDevice 抽象（封装已有 RHI）
- 修改: `core/render/render_pipeline.h` → 拆分为实现类 `RenderForwardBasic`
- 修改: `core/render/CMakeLists.txt` — 添加新文件

### 任务 0.1：定义 RenderingDevice 抽象接口

**文件：**
- 创建: `core/render/rendering_device.h`
- 创建: `core/render/rendering_device.cpp`

```cpp
// rendering_device.h
#pragma once
#include "render/rhi_handle.h"
#include <vector>
#include <string>

namespace gryce_engine::render {

// 从 drivers/vulkan/ 和 drivers/opengl/ 提取公共接口
class RenderingDevice {
public:
    enum class Backend { Vulkan, OpenGL };
    virtual Backend backend() const = 0;
    
    // 纹理
    virtual RHITextureHandle texture_create(...) = 0;
    virtual void texture_free(RHITextureHandle) = 0;
    
    // 缓冲
    virtual RHIBufferHandle buffer_create(...) = 0;
    virtual void buffer_free(RHIBufferHandle) = 0;
    
    // 着色器
    virtual RHIShaderHandle shader_create(...) = 0;
    virtual void shader_free(RHIShaderHandle) = 0;
    
    // Framebuffer
    virtual RHIFramebufferHandle framebuffer_create(...) = 0;
    virtual void framebuffer_free(RHIFramebufferHandle) = 0;
    
    // 绘制
    virtual void draw(...) = 0;
    virtual void clear(...) = 0;
    virtual void present() = 0;
    
    static RenderingDevice* get_singleton();
    static RenderingDevice* create(Backend backend);
};

} // namespace
```

### 任务 0.2：定义 RendererCompositor 抽象

**文件：**
- 创建: `core/render/renderer_compositor.h`
- 创建: `core/render/renderer_compositor.cpp`

```cpp
// renderer_compositor.h
#pragma once
#include <memory>

namespace gryce_engine::render {

class RendererCanvasRender;
class RendererSceneRender;
class RendererFog;
class RendererGI;
class RendererLightStorage;
class RendererMaterialStorage;
class RendererMeshStorage;
class RendererTextureStorage;
class RendererParticlesStorage;
class RendererUtilities;

class RendererCompositor {
public:
    virtual ~RendererCompositor() = default;
    
    virtual RendererCanvasRender* get_canvas() = 0;
    virtual RendererSceneRender* get_scene() = 0;
    virtual RendererFog* get_fog() = 0;
    virtual RendererGI* get_gi() = 0;
    virtual RendererLightStorage* get_light_storage() = 0;
    virtual RendererMaterialStorage* get_material_storage() = 0;
    virtual RendererMeshStorage* get_mesh_storage() = 0;
    virtual RendererTextureStorage* get_texture_storage() = 0;
    virtual RendererParticlesStorage* get_particles_storage() = 0;
    virtual RendererUtilities* get_utilities() = 0;
    
    virtual void initialize() = 0;
    virtual void begin_frame(double frame_step) = 0;
    virtual void end_frame(bool present) = 0;
    virtual void finalize() = 0;
    
    static RendererCompositor* create();
    static RendererCompositor* get_singleton();
};

} // namespace
```

### 任务 0.3：将现有 RenderPipeline 包装为 RenderForwardBasic

**文件：**
- 修改: `core/render/render_pipeline.h` — 保持现有类不变
- 创建: `core/render/render_forward_basic.h` — 继承 RendererSceneRender 的包装器
- 创建: `core/render/render_forward_basic.cpp`

```cpp
// render_forward_basic.h
#pragma once
#include "render/renderer_scene_render.h"
#include "render/render_pipeline.h"

namespace gryce_engine::render {

class RenderForwardBasic : public RendererSceneRender {
public:
    bool init(RenderContext* ctx) override;
    void shutdown() override;
    void render_scene(RenderData* data) override;
    // ... 委托给内部的 RenderPipeline
private:
    std::unique_ptr<RenderPipeline> pipeline_;
};

} // namespace
```

### 任务 0.4：定义 Storage 系统接口

**文件：**
- 创建: `core/render/storage_rd/light_storage.h`
- 创建: `core/render/storage_rd/material_storage.h`
- 创建: `core/render/storage_rd/mesh_storage.h`
- 创建: `core/render/storage_rd/texture_storage.h`
- 创建: `core/render/storage_rd/particles_storage.h`
- 创建: `core/render/storage_rd/utilities.h`

每个 Storage 负责管理对应资源的 RID 分配、生命周期和 GPU 数据同步。接口参考 Godot 的 `RendererRD::LightStorage` 等。

---

## 阶段 1：Canvas Renderer（2D 渲染器）

**前提：** 阶段 0 完成

**目标：** 将现有的 2D 渲染（renderer2d_impl.cpp）分离为独立的 RendererCanvasRender 实现

**文件：**
- 新建: `core/render/renderer_canvas_render_rd.h`
- 新建: `core/render/renderer_canvas_render_rd.cpp`
- 修改: `core/render/renderer2d_impl.h` — 适配新接口

### 任务 1.1：定义 Canvas 渲染接口

Canvas 渲染负责所有 2D 绘制：sprite、tilemap、particle、line、polygon、文本等。

```cpp
// renderer_canvas_render_rd.h
class RendererCanvasRenderRD : public RendererCanvasRender {
public:
    void draw_canvas_item(RID item, const Transform2D& transform) override;
    void canvas_begin() override;
    void canvas_end() override;
    // ...
};
```

### 任务 1.2：迁移现有 2D 渲染逻辑

将 `renderer2d_impl.cpp` 中的 2D 渲染逻辑迁移到 `RendererCanvasRenderRD` 中，同时保留 `vk_renderer2d.cpp` 的 Vulkan 后端调用。

---

## 阶段 2：Forward Clustered 场景渲染器（核心）

**前提：** 阶段 0-1 完成

**目标：** 实现类似 Godot `RenderForwardClustered` 的 3D 渲染管线，包含 Cluster 光剔除、Depth Prepass、Motion Vectors 和完整渲染流程

### 任务 2.1：Cluster Builder（光剔除）

**文件：**
- 新建: `core/render/renderer_rd/cluster_builder_rd.h`
- 新建: `core/render/renderer_rd/cluster_builder_rd.cpp`

Cluster Builder 将视锥体分割为 3D 网格（tile），每个 tile 计算影响它的光源列表。参考 Godot 的 `ClusterBuilderRD`。

```cpp
// cluster_builder_rd.h
struct Cluster {
    std::vector<uint32_t> light_indices;  // 影响本 cluster 的光源索引
    uint32_t offset;                      // 在 cluster buffer 中的偏移
    uint32_t count;                       // 光源数量
};

class ClusterBuilderRD {
public:
    void build(const Camera& camera, const std::vector<Light>& lights,
               int screen_width, int screen_height);
    
    // 返回 cluster 数据缓冲（供 shader 使用）
    RID get_cluster_buffer() const;
    Size3i get_cluster_size() const;
    int get_max_cluster_elements() const;
    
    static constexpr int k_tile_size_x = 32;  // 每个 tile 的像素宽度
    static constexpr int k_tile_size_y = 32;  // 每个 tile 的像素高度
    static constexpr int k_cluster_z_layers = 24; // Z 方向分层数
};
```

### 任务 2.2：Render Forward Clustered 主类

**文件：**
- 新建: `core/render/renderer_rd/forward_clustered/render_forward_clustered.h`
- 新建: `core/render/renderer_rd/forward_clustered/render_forward_clustered.cpp`

```cpp
// render_forward_clustered.h
class RenderForwardClustered : public RendererSceneRenderRD {
public:
    enum RenderListType {
        RENDER_LIST_OPAQUE,
        RENDER_LIST_MOTION,    // 运动向量 pass（供 TAA/FSR2 使用）
        RENDER_LIST_ALPHA,     // 透明物体
        RENDER_LIST_SECONDARY, // 阴影和其他
        RENDER_LIST_MAX
    };
    
    enum PassMode {
        PASS_MODE_COLOR,
        PASS_MODE_SHADOW,
        PASS_MODE_DEPTH,
        PASS_MODE_DEPTH_NORMAL_ROUGHNESS,
        PASS_MODE_DEPTH_MATERIAL,
        PASS_MODE_MOTION_VECTORS,
        PASS_MODE_MAX
    };
    
    void _render_scene(RenderDataRD* data, const Color& clear_color) override;
    // 子步骤：
    void _setup_cluster(const RenderDataRD* data);
    void _render_depth_prepass(RenderDataRD* data);
    void _render_motion_vectors(RenderDataRD* data);
    void _render_opaque_pass(RenderDataRD* data);
    void _render_sky(RenderDataRD* data);
    void _render_alpha_pass(RenderDataRD* data);
    void _render_post_processing(RenderDataRD* data);
};
```

### 任务 2.3：_render_scene 完整流程

实现 `_render_scene` 方法，按 Godot Clustered 的流程：

```
1. _setup_cluster — 构建 Cluster 光剔除
2. _render_shadows — 渲染阴影贴图
3. _render_depth_prepass — Depth Prepass（可选法线+粗糙度）
4. _render_motion_vectors — 运动向量 pass（如需）
5. _render_opaque_pass — 不透明物体 Forward PBR
6. _render_sky — 天空盒
7. _render_alpha_pass — 透明物体
8. _render_post_processing — SSAO → SSR → SSIL → SSS → Bloom → TAA/FSR2 → ToneMap
```

### 任务 2.4：场景 Shader 系统

**文件：**
- 新建: `core/render/renderer_rd/forward_clustered/scene_shader_forward_clustered.h`
- 新建: `core/render/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp`

参考 Godot 的 `SceneShaderForwardClustered`，管理 PBR shader 的各种变体（有无阴影、有无法线贴图、是否透明等）。

```cpp
// scene_shader_forward_clustered.h
class SceneShaderForwardClustered {
public:
    enum ShaderGroup {
        SHADER_GROUP_BASE,
        SHADER_GROUP_ADVANCED,   // 法线/粗糙度/高光分离
        SHADER_GROUP_MULTIVIEW,
        SHADER_GROUP_MOTION_VECTORS,
    };
    
    void enable_group(ShaderGroup group);
    RID get_shader(uint32_t variant_key) const;
};
```

### 任务 2.5：渲染列表管理

实现 4 个渲染列表（OPAQUE/MOTION/ALPHA/SECONDARY）的填充、排序和提交。

每个渲染列表元素包含：mesh 句柄、material、transform、距离（用于透明排序）。

```cpp
struct RenderElement {
    RHIMeshHandle mesh;
    uint32_t material_id;
    math::Matrix4f transform;
    float distance_sq;       // 到相机的距离（透明排序用）
    uint32_t sort_key;       // 排序键（material_id 做位编码）
    uint8_t stencil_value;   // 模板值
};
```

---

## 阶段 3：后处理效果链

**前提：** 阶段 2 完成（Depth Prepass 输出法线/深度）

**目标：** 实现 Godot Clustered 级别的完整后处理链

### 任务 3.1：SSAO（GTAO/地平线搜索）

**文件：**
- 新建: `core/render/renderer_rd/effects/ssao.h`
- 新建: `core/render/renderer_rd/effects/ssao.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/ssao.glsl`
- 新建: `core/render/renderer_rd/shaders/effects/ssao_blur.glsl`

参考 Godot 的 `SSEffects`，实现 GTAO 风格的地平线搜索 AO，半分辨率渲染 + 双边上模糊。

### 任务 3.2：SSR（屏幕空间反射）

**文件：**
- 新建: `core/render/renderer_rd/effects/ssr.h`
- 新建: `core/render/renderer_rd/effects/ssr.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/screen_space_reflection.glsl`

使用 Hierarchical Z-Buffer（HiZ）加速的屏幕空间光线步进，从法线/粗糙度 buffer 计算反射。

### 任务 3.3：SSIL（屏幕空间间接光照）

**文件：**
- 新建: `core/render/renderer_rd/effects/ssil.h`
- 新建: `core/render/renderer_rd/effects/ssil.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/ssil.glsl`

类似 SSAO 但输出彩色间接光照反弹，使用重要性采样 + 多方向。

### 任务 3.4：TAA 升级（运动矢量版）

**文件：**
- 修改: `core/render/render_pipeline.h` — 当前 TAA 为简化版
- 新建: `core/render/renderer_rd/effects/taa_rd.h`
- 新建: `core/render/renderer_rd/effects/taa_rd.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/taa_resolve.glsl`

当前 TAA 使用半像素抖动+邻域钳制。升级为使用运动矢量做 reprojection，参考 Godot 的 TAA 实现。

```cpp
// taa_rd.h
class TAA_RD {
public:
    void resolve(RHITextureHandle current_color, 
                 RHITextureHandle motion_vectors,
                 RHITextureHandle depth,
                 RHITextureHandle history,
                 RHITextureHandle output);
                 
    void set_jitter(uint32_t frame, int width, int height, 
                    float& jitter_x, float& jitter_y);
};
```

### 任务 3.5：Bokeh DOF

**文件：**
- 新建: `core/render/renderer_rd/effects/bokeh_dof.h`
- 新建: `core/render/renderer_rd/effects/bokeh_dof.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/bokeh_dof.glsl`

参考 Godot 的 `BokehDOF`，使用 hexagon bokeh 形状，半分辨率渲染。

### 任务 3.6：FSR2 超分辨率

**文件：**
- 新建: `core/render/renderer_rd/effects/fsr2.h`
- 新建: `core/render/renderer_rd/effects/fsr2.cpp`
- 新建: `core/render/renderer_rd/shaders/effects/fsr2/`

集成 AMD FSR2（开源自包含），需要：运动矢量、深度、曝光、颜色输入。

---

## 阶段 4：Storage 系统实现

**前提：** 阶段 0 接口定义完成

**目标：** 实现 Godot 风格的 Storage 系统，管理所有 GPU 资源

### 任务 4.1：MeshStorage

**文件：**
- 新建: `core/render/storage_rd/mesh_storage_impl.h`
- 新建: `core/render/storage_rd/mesh_storage_impl.cpp`

管理 mesh 的顶点缓冲、索引缓冲、子网格数据。RID 分配。

```cpp
class MeshStorageImpl : public RendererMeshStorage {
    RID mesh_create(...) override;
    void mesh_free(RID) override;
    void mesh_set_vertices(RID, const std::vector<Vertex>&) override;
    void mesh_set_indices(RID, const std::vector<uint32_t>&) override;
    void mesh_set_submesh_count(RID, uint32_t) override;
    // ...
};
```

### 任务 4.2：MaterialStorage

**文件：**
- 新建: `core/render/storage_rd/material_storage_impl.h`
- 新建: `core/render/storage_rd/material_storage_impl.cpp`

管理材质参数、shader 参数、uniform set。

### 任务 4.3：TextureStorage

**文件：**
- 新建: `core/render/storage_rd/texture_storage_impl.h`
- 新建: `core/render/storage_rd/texture_storage_impl.cpp`

管理纹理创建、加载、mipmap、采样器状态。

### 任务 4.4：LightStorage

**文件：**
- 新建: `core/render/storage_rd/light_storage_impl.h`
- 新建: `core/render/storage_rd/light_storage_impl.cpp`

管理光源数据缓冲、阴影贴图、Reflection Probe 缓冲。

---

## 阶段 5：GI 系统（SDFGI + VoxelGI）

**前提：** 阶段 2-4 完成

**目标：** 实现 Godot Clustered 的实时全局光照

### 任务 5.1：SDFGI（有符号距离场 GI）

**文件：**
- 新建: `core/render/renderer_rd/environment/sdfgi.h`
- 新建: `core/render/renderer_rd/environment/sdfgi.cpp`
- 新建: `core/render/renderer_rd/shaders/environment/sdfgi.glsl`

SDFGI 流程：
1. 体素化场景到有符号距离场（SDF）
2. 从 SDF 计算动态 GI 级联
3. 每帧更新光照注入
4. 渲染时从 SDFGI 采样间接光照

### 任务 5.2：VoxelGI

**文件：**
- 新建: `core/render/renderer_rd/environment/voxel_gi.h`
- 新建: `core/render/renderer_rd/environment/voxel_gi.cpp`
- 新建: `core/render/renderer_rd/shaders/environment/voxel_gi.glsl`

VoxelGI 使用体素化的场景数据 + 光照注入，提供静态物体的间接光照。

### 任务 5.3：Fog（体积雾）

**文件：**
- 新建: `core/render/renderer_rd/environment/fog.h`
- 新建: `core/render/renderer_rd/environment/fog.cpp`
- 新建: `core/render/renderer_rd/shaders/environment/volumetric_fog.glsl`

Godot 使用 `VolumetricFog` 类实现体积雾，基于 3D 纹理的 fog 体积。

---

## 阶段 6：Compositor Effect 系统

**前提：** 阶段 2-3 完成

**目标：** 实现 Godot 的 Compositor 回调系统，允许用户插入自定义后处理效果

**文件：**
- 新建: `core/render/compositor_effect.h`
- 新建: `core/render/compositor_effect.cpp`
- 修改: `core/render/renderer_rd/forward_clustered/render_forward_clustered.cpp`

回调点：
- `COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_OPAQUE` — 不透明渲染后
- `COMPOSITOR_EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT` — 透明渲染前
- `COMPOSITOR_EFFECT_CALLBACK_TYPE_POST_TRANSPARENT` — 透明渲染后

---

## 阶段 7：RenderingServer API 层

**前提：** 阶段 0-6 完成

**目标：** 实现类似 Godot `RenderingServer` 的线程安全 API 层

**文件：**
- 新建: `core/render/rendering_server.h`
- 新建: `core/render/rendering_server.cpp`
- 新建: `core/render/rendering_server_default.h`
- 新建: `core/render/rendering_server_default.cpp`

RenderingServer 作为命令缓冲驱动的前端，所有 API 调用通过命令队列异步发送到渲染线程。

```cpp
class RenderingServer {
    // 场景管理
    virtual RID scene_create() = 0;
    virtual void scene_free(RID) = 0;
    virtual void scene_add_node(RID scene, RID node) = 0;
    
    // 实体管理
    virtual RID entity_create() = 0;
    virtual void entity_free(RID) = 0;
    virtual void entity_set_transform(RID, const Transform3D&) = 0;
    virtual void entity_set_mesh(RID, RID mesh) = 0;
    virtual void entity_set_material(RID, RID material) = 0;
    
    // 光源
    virtual RID light_create() = 0;
    virtual void light_set_type(RID, LightType) = 0;
    virtual void light_set_color(RID, const Color&) = 0;
    virtual void light_set_param(RID, LightParam, float) = 0;
    
    // 渲染控制
    virtual void set_viewport(RID, int w, int h) = 0;
    virtual void set_camera(RID, const Camera3D&) = 0;
    virtual void render_frame() = 0;
    
    static RenderingServer* get_singleton();
};
```

---

## 阶段 8：ECS 渲染系统改造

**前提：** 阶段 2-7 完成

**目标：** 将现有的 `render_system_3d.cpp` 改造为通过 RenderingServer API 提交渲染数据

**文件：**
- 修改: `core/ecs/systems/render_system_3d.cpp`
- 修改: `core/ecs/systems/render_system_3d.h`

当前 `render_system_3d.cpp` 直接调用 `RenderPipeline::render_scene`。改造后通过 `RenderingServer` 提交渲染命令。

```cpp
// render_system_3d.cpp
void RenderSystem3D::on_update(Scene& scene, float dt) {
    // 1. 收集可见实体（视锥体剔除）
    // 2. 通过 RenderingServer 提交绘制命令
    // 3. RenderingServer 在渲染线程执行
    
    for (auto* entity : visible_entities) {
        auto* mesh = entity->get_component<MeshComponent>();
        auto* material = entity->get_component<MaterialComponent>();
        auto* transform = entity->get_component<Transform>();
        
        RenderingServer::get_singleton()->entity_set_mesh(entity->id(), mesh->rid);
        RenderingServer::get_singleton()->entity_set_transform(entity->id(), transform->global());
    }
}
```

---

## 阶段 9：场景节点整合

**前提：** 阶段 8 完成

**目标：** 将 ECS 的 Node API（Entity）与 RenderingServer 的 RID 系统打通

**文件：**
- 修改: `core/scene/entity.h`
- 修改: `core/scene/entity.cpp`
- 修改: `core/scene/scene.cpp`

每个 Entity 创建时自动分配 RenderingServer 的 RID，Entity 的 transform 变化自动同步到 RenderingServer。

```cpp
// Entity 构造时
Entity::Entity(const std::string& name) {
    rid_ = RenderingServer::get_singleton()->entity_create();
    RenderingServer::get_singleton()->entity_set_transform(rid_, transform_->global());
}
```

---

## 阶段 10：多后端支持

**前提：** 阶段 0-9 完成

**目标：** 实现类似 Godot 的渲染器切换（Forward Clustered / Forward Mobile / GLES3）

**文件：**
- 修改: `core/render/renderer_compositor.cpp`
- 新建: `core/render/renderer_rd/forward_mobile/render_forward_mobile.h`
- 新建: `core/render/renderer_rd/forward_mobile/render_forward_mobile.cpp`
- 新建: `core/render/renderer_rd/forward_mobile/scene_shader_forward_mobile.h`

通过 `RendererCompositor::create()` 的 `_create_func` 工厂函数切换渲染后端。

```cpp
// 在 core_api.cpp 或项目设置中
void set_renderer(const std::string& name) {
    if (name == "forward_clustered") {
        RendererCompositor::set_create_func(&create_forward_clustered);
    } else if (name == "forward_mobile") {
        RendererCompositor::set_create_func(&create_forward_mobile);
    }
}
```

---

## 执行顺序总览

```
阶段 0: 前置准备 (RendererCompositor 抽象 + RenderingDevice)
    ↓
阶段 1: Canvas Renderer (2D 分离)
    ↓
阶段 2: Forward Clustered (核心渲染管线 + Cluster 剔除)
    ↓
阶段 3: 后处理效果链 (SSAO → SSR → SSIL → TAA → DOF → FSR2)
    ↓
阶段 4: Storage 系统 (Mesh/Material/Texture/Light)
    ↓
阶段 5: GI 系统 (SDFGI + VoxelGI + Fog)
    ↓
阶段 6: Compositor Effect 系统
    ↓
阶段 7: RenderingServer API 层
    ↓
阶段 8: ECS 渲染系统改造
    ↓
阶段 9: 场景节点整合
    ↓
阶段 10: 多后端支持 (Forward Mobile / GLES3)
```

每个阶段完成后都应编译验证。阶段 0-2 完成后即可看到渲染器切换功能，阶段 3-5 逐步增加画质。