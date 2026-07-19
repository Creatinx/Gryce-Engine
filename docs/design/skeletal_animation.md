# 骨骼动画（Skeletal Animation）设计

> 状态：M1-E0 已完成 —— CPU 侧基础 + Assimp skin 导入 + GPU skinning + SkinnedMeshRenderer + AnimatorSystem。
> 动画状态机 / 混合（crossfade）在 M4。

## 1. 目标与边界

- 从 `.fbx` / `.gltf` 导入骨骼（node 层级 + inverse bind matrix）、顶点骨骼权重（每顶点最多 4 影响、归一化）、动画剪辑（关键帧）。
- CPU 侧完成关键帧插值（lerp + slerp）与 pose 求值：local pose → global pose → skinning palette（`std::vector<math::Matrix4f>`）。
- 不产生任何 GPU 调用：全部数据结构为值语义，可在加载工作线程构造、经命令队列按值传递到渲染线程。

## 2. 数据结构（core/animation/，namespace gryce_engine::animation）

### 2.1 Skeleton

```cpp
struct Bone {
    std::string name;
    int32_t parent_index;                 // -1 = 根；bones 按拓扑序存储（父先于子）
    math::Matrix4f inverse_bind_matrix;   // bind pose → bone 空间
    math::Vector3f bind_position;         // bind pose 的 local TRS（动画缺省通道回退用）
    math::Quaternionf bind_rotation;
    math::Vector3f bind_scale{1,1,1};
};
class Skeleton {
    std::vector<Bone> bones;
    int32_t find_bone(const std::string& name) const;  // 未找到 -1
};
```

- 关键设计：bind local TRS 以**分量**存储而非仅存矩阵——glTF/FBX 的动画通道只替换被动画的分量（如仅 translation），未动画分量必须回退到 bind 值，矩阵分解不可靠。

### 2.2 AnimationClip

```cpp
template<typename T> struct Keyframe { float time; T value; };  // time 单位：秒
struct BoneTrack {
    int32_t bone_index;
    std::vector<Keyframe<math::Vector3f>>    position_keys;
    std::vector<Keyframe<math::Quaternionf>> rotation_keys;
    std::vector<Keyframe<math::Vector3f>>    scale_keys;
};
class AnimationClip {
    std::string name;
    float duration;                  // 秒
    std::vector<BoneTrack> tracks;
    math::Matrix4f sample_bone(int32_t bone_index, float time, bool loop) const;
};
```

- 插值：position/scale 线性，rotation 用 `Quaternionf::slerp`（新增于 math，最短路径）。
- 时间换算在导入时完成：`seconds = ticks / (mTicksPerSecond != 0 ? mTicksPerSecond : 1)`，
  运行时 API 一律用秒。越界 clamp；`loop=true` 按 duration 取模。

### 2.3 Pose 求值（core/animation/pose.h，自由函数）

```cpp
Pose            sample_local_pose(const Skeleton&, const AnimationClip* clip, float t, bool loop);
// clip == nullptr → 全部 bone 用 bind TRS
std::vector<math::Matrix4f> compute_global_pose(const Skeleton&, const Pose& local);
// 利用拓扑序：global[i] = global[parent] * local[i]，根为 local[i]
std::vector<math::Matrix4f> compute_skin_palette(const Skeleton&, const std::vector<math::Matrix4f>& global);
// palette[i] = global[i] * inverse_bind_matrix[i]
std::vector<math::Matrix4f> evaluate_skin_palette(const Skeleton&, const AnimationClip* clip, float t, bool loop);
// 一步便捷接口（下一轮 AnimatorSystem 每帧调用）
```

## 3. Assimp 导入（core/assets/assimp_importer.cpp 扩展，不另起炉灶）

新增 `AssimpImporter::import_skinned(path) → SkinnedModelData`（core/assets/skinned_mesh_data.h）：

```cpp
struct BoneInfluence {                       // 每顶点骨骼影响（GPU 友好布局）
    uint16_t bone_ids[4]; float weights[4];  // 权重降序，空槽 weight=0
    void add(int bone_index, float weight);  // 保留 top-4
    void normalize();                        // 权重和归一
};
struct SkinnedMeshData : MeshData {          // 复用现有顶点/索引/材质提取
    std::vector<BoneInfluence> bone_influences;  // 与 vertices 一一对应；空 = 无蒙皮
};
struct SkinnedModelData {
    std::vector<SkinnedMeshData> meshes;
    animation::Skeleton skeleton;
    std::vector<animation::AnimationClip> animations;
    bool has_skin = false;
};
```

导入流程：

1. 与现有 `import()` 相同的 `Assimp::Importer` + post-process 标志（纯 CPU，无线程/GPU 依赖）。
2. **Skeleton**：收集所有 mesh 的 `aiBone::mName` 并集 → 标记被引用节点及其全部祖先为骨骼 → 先序遍历分配 index（天然拓扑序）→ bind TRS 由 `aiNode::mTransformation.Decompose` 得到 → `aiBone::mOffsetMatrix` 直接作为 inverse bind matrix；无 `aiBone` 的结构祖先节点用 `inverse(global_bind)` 补齐。`aiMatrix4x4`（行主序）→ `Matrix4f`（列主序）显式转置转换。
3. **权重**：`aiBone::mWeights` 的 `(mVertexId, mWeight)` 填入 `BoneInfluence::add`（top-4 截断），全部填完后逐顶点 `normalize()`。
4. **动画**：`aiAnimation` 的每个 `aiNodeAnim` channel 按名字解析 bone index（非骨骼 channel 跳过），关键帧时间换算为秒；旋转 `aiQuaternion(w,x,y,z)` → `Quaternionf(x,y,z,w)` 并归一化。

## 4. 异步上传路径（已实现）

线程模型沿用既有架构：主线程提交命令、渲染线程执行；工作线程只做纯 CPU 工作。

```
[AsyncLoader worker]                 [主线程]                      [渲染线程]
import_skinned (纯 CPU)      →      poll() 回调收 SkinnedModelData  →
值语义数据跨线程移动                 push_command（by-value 捕获）    → GPU 上传顶点+权重
                                    alive_token 校验组件存活
AnimatorSystem 每帧 evaluate_skin_palette → set_uniform_mat4_array
（shared_ptr<palette> 按值捕获）                              → 写 uniform/UBO
```

约束（来自最近四轮 bug 修复成果，必须遵守）：

- 进命令队列的 lambda **按值捕获**（`shared_ptr<std::vector<Matrix4f>>` 等），禁止引用/裸指针捕获栈变量。
- 组件生命周期用 `MeshRenderer::alive_token()` 同款 `shared_ptr<atomic<bool>>` 模式，回调执行前校验。
- GPU 资源一律 RHI 句柄 + generation 校验；纹理上传只走渲染线程（参考 vk `create_texture_from_data` 最新实现）。
- `import_skinned` 不触碰任何 GL/VK 对象，可直接在 AsyncLoader 工作线程运行。

## 5. GPU skinning 实现（E0，已完成）

### 5.1 顶点布局（core/render/skinned_vertex.h）

- `SkinnedVertexGPU`（88B）：MeshVertex 56B + `bone_ids[4]`（uint32，location 5）+ `weights[4]`（float，location 6）。
- `VertexType` 新增 `Int4/UInt4`；GL 端整数属性走 `glVertexArrayAttribIFormat` / `glVertexAttribIPointer`（`is_integer_vertex_type` 判定），VK 端 `VK_FORMAT_R32G32B32A32_UINT`。
- `build_skinned_vertices(SkinnedMeshData)` 打包交错顶点；influences 缺失/数量不符回退全零（shader 侧权重和 0 → 单位阵，顶点原位）。
- 普通 mesh 路径不受影响：SkinnedMeshRenderer 是独立组件 + 独立 layout。

### 5.2 Shader（GLSL + SPIR-V 两端）

- `skinned_pbr.vert`（GLSL 330）：`uniform mat4 uBonePalette[128]`，LBS `Σ wᵢ·palette[idᵢ]`，normal/tangent 同矩阵蒙皮。
- `vulkan_skinned_pbr.vert`（GLSL 450）：palette 走 `set=0, binding=8` UBO；4×mat4 push constants 与 vulkan_pbr 一致。
- `.frag` 与 pbr/vulkan_pbr 完全一致（直接复制）。骨骼上限 128（`render::k_max_skinning_bones`），加载时超限告警、求值时截断。

### 5.3 palette 上传机制

- `IShader::set_mat4_array(name, data, count)`：GL → `glUniformMatrix4fv`；VK → 写入 palette 缓存（`prepare_draw` 时上传每帧 palette UBO，binding 8，与主 UBO 共用 draw cursor，stride 8192B，上限 256 skinned draws/帧）。
- `RenderContext::set_uniform_mat4_array(shader, name, shared_ptr<vector<Matrix4f>>)`：shared_ptr 按值捕获进命令队列，渲染线程经 backend 句柄解析 shader 后写入。`load_program` 新增 `skinned` 参数构建 VK 蒙皮管线（顶点布局 + 描述符 binding 8）。
- RenderPipeline 启动时加载可选的 `skinned_pbr` shader（缺失只告警降级，不破坏旧项目）。

### 5.4 组件与系统

- `SkinnedMeshRenderer`（core/components/）：model_path + material + 播放状态（clip_name/playing/loop/speed/time）；`set_model`（AssetManager 缓存的 shared_ptr）、`set_palette`（每帧新分配的 shared_ptr，主线程替换指针对已入队命令无影响）；`upload_to_gpu` 复用 alive_token + 渲染线程上传模式。本轮只上传第一个带 skin 的 mesh（多 mesh 模型为遗留项）。
- `AnimatorSystem`（core/ecs/systems/，Phase::Update）：懒加载模型 → 推进时间 → `evaluate_skin_palette` → 截断到 128 → 注入组件。clip 切换最小支持（改 clip_name 即生效）；状态机/混合在 M4。
- `RenderSystem3D`：SkinnedMeshRenderer 的 GPU 上传走与 MeshRenderer 相同的 pending + alive_token + push_command 路径。
- `RenderPipeline::render_scene`：SkinnedMeshRenderer 实体走 `render_skinned_mesh`（独立收集 + 不透明/透明两阶段，与普通 mesh 一致）。蒙皮网格暂不参与 shadow pass（遗留项）。

### 5.5 AssetManager 集成

- `load_skinned_model(path)`：同步缓存加载（import_skinned，GRYCE_HAS_ASSIMP 保护），超 128 骨告警。
- `load_skinned_model_async(path, cb)`：AsyncLoader 工作线程导入 → `poll()` 主线程回调；demo 使用该路径。

## 6. 演示与测试

- 演示：`examples/demo_skinned3d`（GL/VK 均可），加载 `skinned_triangle.gltf`，AnimatorSystem 播放 loop 动画；空格暂停/继续。
- 测试：`tests/skinned_renderer_test.cpp` 8 用例（顶点打包/布局契约/AnimatorSystem 时间推进/palette 截断/禁用跳过/暂停求值）。

## 7. 遗留项

- 多 mesh 蒙皮模型（当前只上传第一个带 skin 的 mesh）。
- 蒙皮网格的 shadow pass（需要蒙皮版 shadow shader + palette 绑定）。
- palette 每帧堆分配（可换环形缓冲池优化）。
- 动画混合 / 状态机（M4）。

## 8. 集成点

| 模块 | 集成方式 |
|---|---|
| math | 新增 `Quaternionf::slerp`（最短路径），非侵入 |
| AssimpImporter | 新增 `import_skinned`，现有 `import()` 不变；`GRYCE_HAS_ASSIMP` 宏保护 |
| MeshData | `SkinnedMeshData` 继承扩展，现有 mesh 路径（顶点布局/上传）不受影响 |
| AssetManager | `load_skinned_model` / `load_skinned_model_async`（独立缓存 map，不动 Asset 表） |
| MeshRenderer | 不变；SkinnedMeshRenderer 为独立组件，复用 alive_token / upload_to_gpu 模式 |
| RenderPipeline | 新增可选 `skinned_pbr` shader + `render_skinned_mesh`；缺失时降级不报错 |

## 9. 后续方向（原「下一轮预告」，已完成的见第 5 节）

- ~~顶点布局扩展：location 5 = bone ids（UInt4），location 6 = weights（Float4）~~（已完成）
- ~~palette 经 UBO（GL/VK 各后端）传入顶点着色器；shader 内 `Σ w_i * palette[id_i]` 蒙皮~~（已完成）
- ~~`SkinnedMeshRenderer` + `AnimatorSystem`~~（已完成）
- M4：动画状态机、crossfade 混合、骨骼调试绘制（editor 面板依赖 M1）。
