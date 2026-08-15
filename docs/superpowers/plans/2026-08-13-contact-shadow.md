# 屏幕空间接触阴影（Contact Shadow）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在引擎中加入 UE 风格的屏幕空间接触阴影，在物体落地处补黑，消除实时阴影的 Peter-Panning（悬浮）亮缝。

**Architecture:** 引擎是前向渲染，深度 `hdr_depth_` 在主 pass 才写入。为性能优先，接触阴影做成**后处理 pass**：在主 pass 之后用最终深度半分辨率生成一张屏幕空间接触阴影因子贴图，再在 tonemap 阶段乘到 HDR 颜色上。不重复绘制场景（性能优先），只局部补黑接触处。

**Tech Stack:** C++（RenderPipeline）、GLSL（OpenGL 后端）、ImGui（调试面板）。

---

## 文件结构

| 操作 | 路径 | 职责 |
|------|------|------|
| Create | `examples/FPSDemo/shaders/contact_shadow.frag` | 接触阴影计算（读深度，沿方向光屏幕空间步进） |
| Modify | `examples/FPSDemo/shaders/tonemap.frag` | 采样接触阴影贴图，乘到 HDR 颜色 |
| Modify | `core/render/texture.h` | 新增纹理槽位 `kTonemapContactShadow = 34` |
| Modify | `core/render/render_pipeline.h` | 声明设置接口、成员、私有方法 |
| Modify | `core/render/render_pipeline.cpp` | 创建/销毁 target、渲染 pass、init/shutdown 接线、render_scene 调用、tonemap 绑定 |
| Modify | `examples/common/ui/debug_panel.cpp` | 开关 + 参数 UI |

> 说明：着色器按项目目录加载（`res:/shaders`）。`contact_shadow.frag` 与 `tonemap.frag` 需在 `examples/3dtest/shaders`、`examples/2dDemo/shaders`、`examples/FPSDemo/shaders` 三处保持一致（Task 9 处理）。

---

### Task 1: 新增 `contact_shadow.frag` 着色器

**Files:**
- Create: `examples/FPSDemo/shaders/contact_shadow.frag`

- [ ] **Step 1: 编写接触阴影着色器**

在 `examples/FPSDemo/shaders/contact_shadow.frag` 写入：

```glsl
#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// 场景深度（主 pass 写入，Depth24）
uniform sampler2D uDepthTexture;
// 相机参数：由 render_contact_shadow() 用 set_uniform_float 传入
uniform float uCSNear;
uniform float uCSFar;
uniform float uCSTanHalfFov;
uniform float uCSAspect;
// 方向光方向（视图空间，指向光源），由 set_uniform_vec3 传入
uniform vec3 uCSLightDirView;
uniform float uCSRadius;   // 接触阴影世界半径（越小越只影响脚底接触处）
uniform int uCSteps;       // 步进数
uniform float uCSStrength; // 强度
uniform int uCSEnabled;    // 0 直接输出全亮

float linearize_depth(float d) {
    // 深度纹理存的是视图变换后的 [0,1] 深度，直接反算线性深度（勿再 *0.5+0.5）
    float d01 = d;
    return (2.0 * uCSNear * uCSFar) /
           (uCSFar + uCSNear - d01 * (uCSFar - uCSNear));
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * uCSTanHalfFov * uCSAspect * lin,
                ndc.y * uCSTanHalfFov * lin,
                -lin);
}

void main() {
    if (uCSEnabled == 0) { FragColor = vec4(1.0); return; }
    float lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    if (lin < 0.01 || lin >= uCSFar * 0.99) { FragColor = vec4(1.0); return; }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);
    vec3 L = normalize(uCSLightDirView);
    float step_w = uCSRadius / float(max(uCSteps, 1));
    float occ = 0.0;
    for (int s = 1; s <= uCSteps; ++s) {
        vec3 Ps = P + L * (step_w * float(s));
        // 视图点投影回屏幕 uv
        vec2 proj = Ps.xy / (-Ps.z * uCSTanHalfFov);
        vec2 suv = proj * vec2(0.5 / uCSAspect, 0.5) + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        float lin2 = linearize_depth(texture(uDepthTexture, suv).r);
        if (lin2 < 0.01 || lin2 >= uCSFar * 0.99) continue;
        vec3 Q = reconstruct_view_pos(suv, lin2);

        // 视图空间 z 为负：Q 比 Ps 更靠近相机（Q.z > Ps.z）说明该方向有几何 → 接触遮挡
        if (Q.z > Ps.z + 1e-4) {
            occ += 1.0;
        }
    }
    float cs = 1.0 - (occ / float(max(uCSteps, 1))) * uCSStrength;
    FragColor = vec4(vec3(clamp(cs, 0.0, 1.0)), 1.0);
}
```

- [ ] **Step 2: 提交**

```bash
git add examples/FPSDemo/shaders/contact_shadow.frag
git commit -m "feat(shadow): add screen-space contact shadow fragment shader"
```

---

### Task 2: 新增纹理槽位

**Files:**
- Modify: `core/render/texture.h:74`

- [ ] **Step 1: 在 `kMaxTextureSlot` 之前新增槽位**

在 `core/render/texture.h` 的 `kTAAHistory = 32` / `kPBRSSAO = 33` 之后追加：

```cpp
    constexpr int kTonemapContactShadow = 34; // 屏幕空间接触阴影（半分辨率）
```

- [ ] **Step 2: 提交**

```bash
git add core/render/texture.h
git commit -m "feat(shadow): add contact shadow texture slot"
```

---

### Task 3: `render_pipeline.h` 声明接口与成员

**Files:**
- Modify: `core/render/render_pipeline.h`

- [ ] **Step 1: 在 SSAO 区块（`set_ssao_params` 之后）新增公开接口**

在 `core/render/render_pipeline.h` 的 `set_ssao_params(...)` 声明之后插入：

```cpp
    // -----------------------------------------------------------------------
    // 屏幕空间接触阴影（Contact Shadow）：主 pass 后沿方向光方向半分辨率
    // 步进采样深度，补落地悬浮（Peter-Panning）脚底的黑。默认关闭。
    // -----------------------------------------------------------------------
    void set_contact_shadow_enabled(bool enabled) { contact_shadow_enabled_ = enabled; }
    bool contact_shadow_enabled() const { return contact_shadow_enabled_; }
    void set_contact_shadow_params(float strength, float radius_world, int steps);
```

- [ ] **Step 2: 在私有方法区（`render_ssao` 声明附近）新增方法声明**

```cpp
    bool create_contact_shadow_targets(RenderContext* ctx);
    void destroy_contact_shadow_targets();
    void render_contact_shadow(RenderContext& ctx);
```

- [ ] **Step 3: 在成员区（`ssao_fallback_tex_` 之后）新增成员**

```cpp
    // 屏幕空间接触阴影
    RHITextureHandle contact_shadow_tex_;
    RHIFramebufferHandle contact_shadow_fbo_;
    RHIShaderHandle contact_shadow_shader_;
    int cs_w_ = 0;
    int cs_h_ = 0;
    bool contact_shadow_enabled_ = false;
    float contact_shadow_strength_ = 0.6f;
    float contact_shadow_radius_ = 0.5f;   // 世界单位
    int contact_shadow_steps_ = 4;
    bool contact_shadow_targets_valid_ = false;
```

- [ ] **Step 4: 提交**

```bash
git add core/render/render_pipeline.h
git commit -m "feat(shadow): declare contact shadow API and members"
```

---

### Task 4: 实现 target 创建/销毁与渲染 pass

**Files:**
- Modify: `core/render/render_pipeline.cpp`

- [ ] **Step 1: 实现 `create_contact_shadow_targets`（紧邻 `create_ssao_targets` 之后）**

```cpp
bool RenderPipeline::create_contact_shadow_targets(RenderContext* ctx) {
    cs_w_ = std::max(16, viewport_width_ / 2);
    cs_h_ = std::max(16, viewport_height_ / 2);
    contact_shadow_tex_ = ctx->create_texture();
    ITexture* tex = ctx->texture(contact_shadow_tex_);
    if (!contact_shadow_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, cs_w_, cs_h_, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    contact_shadow_fbo_ = ctx->create_framebuffer();
    IFramebuffer* fbo = ctx->framebuffer(contact_shadow_fbo_);
    if (!contact_shadow_fbo_.is_valid() || !fbo || !fbo->create(cs_w_, cs_h_)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;
    contact_shadow_targets_valid_ = true;
    return true;
}

void RenderPipeline::destroy_contact_shadow_targets() {
    if (!ctx_) return;
    if (contact_shadow_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(contact_shadow_fbo_);
        contact_shadow_fbo_ = RHIFramebufferHandle{};
    }
    if (contact_shadow_tex_.is_valid()) {
        ctx_->destroy_texture(contact_shadow_tex_);
        contact_shadow_tex_ = RHITextureHandle{};
    }
    contact_shadow_targets_valid_ = false;
}
```

- [ ] **Step 2: 实现 `render_contact_shadow`（紧邻 `render_ssao` 之后）**

```cpp
void RenderPipeline::render_contact_shadow(RenderContext& ctx) {
    if (!contact_shadow_enabled_ || !contact_shadow_targets_valid_) return;
    if (!contact_shadow_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;
    if (!camera_) return;

    const float near_p = camera_->near_plane();
    const float far_p = camera_->far_plane();
    const float tan_half = std::tan(math::to_radians(camera_->fov()) * 0.5f);
    const float aspect = camera_->aspect();

    // 取第一个方向光，步进方向指向光源（-direction）
    math::Vector3f light_dir = math::Vector3f(0.0f, -1.0f, 0.0f);
    for (const auto& l : lights_) {
        if (l.type == LightType::Directional) { light_dir = l.direction; break; }
    }
    const math::Matrix4f view = camera_->get_view_matrix();
    const math::Vector3f light_dir_view = view.transform_vector(-light_dir);

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    ctx.set_framebuffer(contact_shadow_fbo_);
    ctx.set_viewport(0, 0, cs_w_, cs_h_);
    ctx.set_shader(contact_shadow_shader_);
    ctx.set_texture_raw_depth(contact_shadow_shader_, hdr_depth_, TextureSlots::kTonemapHDR, "uDepthTexture");
    ctx.set_uniform_int(contact_shadow_shader_, "uDepthTexture", TextureSlots::kTonemapHDR);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSNear", near_p);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSFar", far_p);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSTanHalfFov", tan_half);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSAspect", aspect);
    ctx.set_uniform_vec3(contact_shadow_shader_, "uCSLightDirView", light_dir_view);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSRadius", contact_shadow_radius_);
    ctx.set_uniform_int(contact_shadow_shader_, "uCSteps", contact_shadow_steps_);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSStrength", contact_shadow_strength_);
    ctx.set_uniform_int(contact_shadow_shader_, "uCSEnabled", 1);
    ctx.draw_mesh(fullscreen_mesh_, contact_shadow_shader_);
}
```

- [ ] **Step 3: 实现 `set_contact_shadow_params`（紧邻 `set_ssao_params` 实现之后）**

```cpp
void RenderPipeline::set_contact_shadow_params(float strength, float radius_world, int steps) {
    contact_shadow_strength_ = math::clamp(strength, 0.0f, 2.0f);
    contact_shadow_radius_ = std::max(0.01f, radius_world);
    contact_shadow_steps_ = std::clamp(steps, 1, 32);
}
```

- [ ] **Step 4: 提交**

```bash
git add core/render/render_pipeline.cpp
git commit -m "feat(shadow): implement contact shadow target and render pass"
```

---

### Task 5: init / shutdown 接线

**Files:**
- Modify: `core/render/render_pipeline.cpp`

- [ ] **Step 1: 在 `init()` 的 `create_ssao_targets` 分支之后追加接触阴影初始化**

在 `core/render/render_pipeline.cpp` 的 `init()` 中，`create_ssao_targets(ctx)` 的 `if/else` 块之后追加：

```cpp
        if (create_contact_shadow_targets(ctx)) {
            contact_shadow_shader_ = load_shader("contact_shadow", contact_shadow_fbo_, true, true);
            if (!contact_shadow_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: contact shadow shader unavailable, disabled");
                contact_shadow_enabled_ = false;
            }
        } else {
            GLOG_WARN("RenderPipeline: contact shadow targets failed, disabled");
            contact_shadow_enabled_ = false;
        }
```

- [ ] **Step 2: 在 `shutdown()` 中销毁 shader 与 target**

在 `shutdown()` 中（`ssao_blur_shader_` 销毁之后）追加：

```cpp
    if (owns_shaders_ && contact_shadow_shader_.is_valid()) {
        ctx_->destroy_shader(contact_shadow_shader_);
        contact_shadow_shader_ = RHIShaderHandle{};
    }
    destroy_contact_shadow_targets();
```

- [ ] **Step 3: 提交**

```bash
git add core/render/render_pipeline.cpp
git commit -m "feat(shadow): wire contact shadow into init and shutdown"
```

---

### Task 6: render_scene 调用 + render_tonemap 绑定

**Files:**
- Modify: `core/render/render_pipeline.cpp`

- [ ] **Step 1: 在 `render_scene` 的 `render_ssao(ctx)` 之后调用接触阴影 pass**

在 `core/render/render_pipeline.cpp` 的 `render_scene()` 中，`render_ssao(ctx);` 之后追加：

```cpp
        // 3a. 屏幕空间接触阴影（补 Peter-Panning 脚底黑）
        render_contact_shadow(ctx);
```

- [ ] **Step 2: 在 `render_tonemap` 中选绑定接触阴影贴图并传强度**

在 `render_tonemap()` 中，绑定曝光纹理之后追加：

```cpp
    // 屏幕空间接触阴影：半分辨率因子贴图，乘到 HDR 颜色（禁用时保持全亮）
    if (contact_shadow_enabled_ && contact_shadow_targets_valid_ && contact_shadow_tex_.is_valid()) {
        ctx.set_texture(tonemap_shader_, contact_shadow_tex_, TextureSlots::kTonemapContactShadow, "uContactShadowTexture");
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowTexture", TextureSlots::kTonemapContactShadow);
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowEnabled", 1);
        ctx.set_uniform_float(tonemap_shader_, "uContactShadowStrength", contact_shadow_strength_);
    } else {
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowEnabled", 0);
    }
```

- [ ] **Step 3: 提交**

```bash
git add core/render/render_pipeline.cpp
git commit -m "feat(shadow): invoke contact shadow pass and bind in tonemap"
```

---

### Task 7: `tonemap.frag` 采样并乘到 HDR 颜色

**Files:**
- Modify: `examples/FPSDemo/shaders/tonemap.frag`

- [ ] **Step 1: 声明 uniform**

在 `tonemap.frag` 的 uniform 区（`uChromaticAberration` 之后）追加：

```glsl
uniform sampler2D uContactShadowTexture;
uniform int uContactShadowEnabled = 0;
uniform float uContactShadowStrength = 0.6;
```

- [ ] **Step 2: 在 bloom 合成之后、tonemap 之前乘接触阴影**

在 `tonemap.frag` 的 `main()` 中，bloom 合成块（`if (uBloomEnabled != 0) {...}`）之后，`vec3 ldr;` 之前插入：

```glsl
    // 屏幕空间接触阴影：补物体落地处的 Peter-Panning 悬浮亮缝
    if (uContactShadowEnabled != 0) {
        float cs = texture(uContactShadowTexture, vTexCoord).r;
        hdr *= mix(1.0, cs, uContactShadowStrength);
    }
```

- [ ] **Step 3: 提交**

```bash
git add examples/FPSDemo/shaders/tonemap.frag
git commit -m "feat(shadow): apply contact shadow in tonemap pass"
```

---

### Task 8: 调试面板开关与参数

**Files:**
- Modify: `examples/common/ui/debug_panel.cpp`

- [ ] **Step 1: 在 PCSS 参数区之后新增接触阴影 UI**

在 `debug_panel.cpp` 的 PCSS 相关控件之后追加：

```cpp
    // 屏幕空间接触阴影（补 Peter-Panning 脚底黑）
    static bool contact_shadow_enabled = pipeline && pipeline->contact_shadow_enabled();
    static float cs_strength = 0.6f;
    static float cs_radius = 0.5f;
    static int cs_steps = 4;
    if (ImGui::Checkbox("Contact Shadow", &contact_shadow_enabled)) {
        if (pipeline) pipeline->set_contact_shadow_enabled(contact_shadow_enabled);
    }
    if (ImGui::SliderFloat("CS Strength", &cs_strength, 0.0f, 1.0f, "%.2f")) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }
    if (ImGui::SliderFloat("CS Radius", &cs_radius, 0.05f, 2.0f, "%.2f")) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }
    if (ImGui::SliderInt("CS Steps", &cs_steps, 1, 16)) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }
```

- [ ] **Step 2: 提交**

```bash
git add examples/common/ui/debug_panel.cpp
git commit -m "feat(shadow): add contact shadow controls to debug panel"
```

---

### Task 9: 同步着色器到其他示例 + 构建验证

**Files:**
- Copy: `contact_shadow.frag` → `examples/3dtest/shaders/`、`examples/2dDemo/shaders/`
- Copy: `tonemap.frag` 修改 → `examples/3dtest/shaders/`、`examples/2dDemo/shaders/`

- [ ] **Step 1: 复制 `contact_shadow.frag` 到另两个示例**

将 `examples/FPSDemo/shaders/contact_shadow.frag` 复制到 `examples/3dtest/shaders/` 与 `examples/2dDemo/shaders/`。

- [ ] **Step 2: 对 `3dtest` / `2dDemo` 的 `tonemap.frag` 应用与 Task 7 相同的两处修改**

- [ ] **Step 3: 构建验证**

```bash
python build.py
```

Expected: 编译通过，无错误。若报 `TextureSlots::kTonemapContactShadow` 未定义，确认 Task 2 已提交。

- [ ] **Step 4: 运行 FPSDemo 验证**

运行 `build/Debug/bin/Debug/FPSDemo.exe`，在调试面板开启 `Contact Shadow`，观察角色/物体落地处亮缝是否被补黑。若悬浮仍然可见，调大 `CS Strength` 或 `CS Radius`；若出现大范围误黑，调小 `CS Radius`。

- [ ] **Step 5: 提交**

```bash
git add examples/3dtest/shaders examples/2dDemo/shaders
git commit -m "feat(shadow): sync contact shadow shaders to all examples"
```

---

## Self-Review

**Spec coverage:** 目标（消除悬浮亮缝）由 Task 1/6/7 实现；性能优先（不重复绘制）由"后处理单 pass、半分辨率"保证；可调性由 Task 8 提供。无遗漏。

**Placeholder scan:** 所有代码步骤均含完整代码，无 "TBD/TODO/类似 Task N"。

**Type consistency:** `contact_shadow_enabled_/strength_/radius_/steps_`、`cs_w_/cs_h_`、`contact_shadow_tex_/fbo_/shader_`、`create/destroy_contact_shadow_targets`、`render_contact_shadow`、`set_contact_shadow_params` 在头文件声明与 cpp 实现中名称一致。`kTonemapContactShadow` 在 texture.h 定义，cpp 中引用。