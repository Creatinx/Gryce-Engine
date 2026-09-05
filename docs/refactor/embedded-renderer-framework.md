# Gryce Engine → 嵌入式渲染器框架 改造方案

> 将现有的 Gryce Engine（游戏引擎）改造为 GryceEngineUtils（游戏框架 + 嵌入式渲染器）。
> 渲染器是核心，ECS/物理/音频是可选模块，所有 API 归入 `GryceEngineUtils` 命名空间。

---

## 一、头文件结构

```
include/  ← 新增，作为公共头文件目录
└── GryceEngineUtils/
    ├── renderer.h                 ← GryceEngineUtils::Renderer
    ├── math.h                     ← re-export core/math/ 到 GryceEngineUtils::math::
    ├── types.h                    ← 基础类型定义
    ├── physics.h                  ← GryceEngineUtils::PhysicsWorld
    ├── audio.h                    ← GryceEngineUtils::AudioEngine
    ├── ecs/
    │   ├── world.h                ← GryceEngineUtils::ecs::World
    │   ├── scene.h                ← GryceEngineUtils::ecs::Scene
    │   ├── entity.h               ← GryceEngineUtils::ecs::Entity
    │   └── component.h            ← GryceEngineUtils::ecs::Component
    └── ui/                        ← 独立 UI 模块
        ├── ui.h                   ← GryceEngineUtils::ui::（主入口，包含所有 Widget）
        ├── widget.h               ← Widget 基类
        ├── signal.h               ← 信号系统
        ├── stylesheet.h           ← 样式表解析器
        ├── animation.h            ← 动画系统
        ├── factory.h              ← WidgetFactory（按类型名快速创建控件）
        ├── label.h                ← Label 控件
        ├── button.h               ← Button 控件
        ├── panel.h                ← Panel 容器
        ├── toolbar.h              ← ToolBar 工具栏
        ├── menubar.h              ← MenuBar 菜单栏 + MenuItem 菜单项
        ├── divider.h              ← Divider 分隔线
        ├── combobox.h             ← ComboBox 下拉选择框
        ├── radiobutton.h          ← RadioButton 单选按钮
        ├── spinbox.h              ← SpinBox 数值微调框
        ├── image.h                ← Image 控件
        ├── text_input.h           ← TextInput 控件
        ├── slider.h               ← Slider 控件
        ├── checkbox.h             ← CheckBox 控件
        ├── progress_bar.h         ← ProgressBar 控件
        └── scroll_view.h          ← ScrollView 容器
```

---

## 二、核心 Renderer API

```cpp
// GryceEngineUtils/renderer.h
namespace GryceEngineUtils {

struct RendererConfig {
    const char* title = "Gryce";
    int width = 1280, height = 720;
    bool vsync = true;
    bool hdr = true;
    int shadow_map_size = 2048;
    RenderAPI api = RenderAPI::OpenGL;
};

class Renderer {
public:
    static Renderer* create(const RendererConfig& config);
    void destroy();

    // 帧控制
    void begin_frame();
    void end_frame();
    bool is_running();
    double delta_time() const;

    // 直接提交绘制（核心！不依赖 Scene/Entity）
    void draw(IMesh* mesh, IMaterial* material,
              const math::Matrix4f& transform);
    void draw_instanced(IMesh* mesh, IMaterial* material,
                        const math::Matrix4f* transforms, int count);

    // 相机与光源
    void set_camera(const math::Vector3f& position,
                    const math::Matrix4f& view_proj);
    void set_lights(const LightData* lights, int count);
    void set_ambient(const math::Vector3f& color);

    // 资源管理
    IMesh*      load_mesh(const char* path);
    IMesh*      create_mesh(const float* vertices, int vcount,
                            const uint32_t* indices, int icount);
    IMaterial*  load_material(const char* path);
    IMaterial*  create_material();
    ITexture*   load_texture(const char* path);
    IShader*    load_shader(const char* vert_path, const char* frag_path);

    // 效果开关
    void set_shadow_enabled(bool on);
    void set_ssr_enabled(bool on);
    void set_dof_enabled(bool on);
    void set_motion_blur_enabled(bool on);
    void set_fog_enabled(bool on);
    void set_water_enabled(bool on);
    void set_gi_enabled(bool on);

    // 高级访问（需要时直接操作底层）
    RenderPipeline* pipeline();
    RenderContext*  context();
    void*           native_window();
};

} // namespace GryceEngineUtils
```

---

## 三、物理 & 音频 API

```cpp
// GryceEngineUtils/physics.h
namespace GryceEngineUtils {

class PhysicsWorld {
public:
    static PhysicsWorld* create();
    void destroy();
    void step(float dt);
    // 后续添加：raycast, add_body, remove_body, set_gravity 等
};

} // namespace GryceEngineUtils
```

```cpp
// GryceEngineUtils/audio.h
namespace GryceEngineUtils {

class AudioEngine {
public:
    static AudioEngine* create();
    void destroy();
    // 后续添加：play_sound, set_volume, set_listener 等
};

} // namespace GryceEngineUtils
```

---

## 四、UI API

```cpp
// GryceEngineUtils/ui/ui.h —— 主入口
namespace GryceEngineUtils::ui {

// ============================================================
// 1. Widget 基类（所有 UI 控件的根）
// ============================================================
class Widget {
public:
    Widget(const char* id = nullptr);
    virtual ~Widget();

    // 布局
    void set_position(float x, float y);
    void set_size(float w, float h);
    void set_anchor(Anchor anchor);          // 锚点：TopLeft, Center, Stretch 等
    void set_margin(const Margin& m);
    void set_padding(const Padding& p);

    // 父子关系
    void add_child(Widget* child);
    void remove_child(Widget* child);
    Widget* parent() const;
    const std::vector<Widget*>& children() const;

    // 可见性
    void set_visible(bool visible);
    bool visible() const;
    void set_opacity(float opacity);         // 0~1

    // 样式
    void set_style(const Style& style);
    void set_style_class(const char* cls);
    void set_style_id(const char* id);
    const Style& computed_style() const;     // 级联计算后的最终样式

    // 事件（信号）
    Signal<void(Widget*)> on_click;
    Signal<void(Widget*, bool)> on_hover;    // entered/left
    Signal<void(Widget*)> on_focus;
    Signal<void(Widget*)> on_blur;

    // 渲染（框架内部调用）
    virtual void draw(Renderer* renderer);
    virtual void update(float dt);           // 驱动动画

    // 命中测试
    virtual bool hit_test(float x, float y) const;

protected:
    Rect bounds_;          // 实际屏幕区域（由布局系统计算）
    Style style_;          // 当前样式
    std::vector<Widget*> children_;
    Widget* parent_ = nullptr;
    // ...
};

// ============================================================
// 2. 内置控件
// ============================================================
class Label : public Widget {
public:
    Label(const char* id, const char* text);
    void set_text(const char* text);
    void set_font_size(float size);
    void set_color(const Color& color);
    void set_text_align(TextAlign align);    // Left, Center, Right
    void draw(Renderer* renderer) override;
};

class Button : public Widget {
public:
    Button(const char* id, const char* text);
    // 信号
    Signal<void()> on_pressed;
    Signal<void()> on_released;
    // 状态
    bool is_pressed() const;
    bool is_hovered() const;
    // 样式（可通过 style sheet 覆盖）
    void set_background(const Color& normal, const Color& hover, const Color& pressed);
    void set_border_radius(float r);
    void draw(Renderer* renderer) override;
    void update(float dt) override;
};

class Panel : public Widget {
public:
    Panel(const char* id);
    void set_layout(LayoutType type);        // Vertical, Horizontal, Grid, Absolute
    void set_spacing(float gap);
    void set_background(const Color& color);
    void draw(Renderer* renderer) override;
};

class Image : public Widget {
public:
    Image(const char* id);
    void set_texture(ITexture* tex);
    void set_image_scale(ImageScale scale);  // Fill, Fit, Stretch, None
    void set_tint(const Color& color);
    void draw(Renderer* renderer) override;
};

class TextInput : public Widget {
public:
    TextInput(const char* id);
    Signal<void(const char*)> on_text_changed;
    Signal<void()> on_submit;               // Enter 键
    void set_placeholder(const char* text);
    void set_max_length(int len);
    const char* text() const;
    void draw(Renderer* renderer) override;
};

class Slider : public Widget {
public:
    Slider(const char* id);
    Signal<void(float)> on_value_changed;   // 0~1
    void set_range(float min, float max);
    void set_value(float val);
    float value() const;
    void draw(Renderer* renderer) override;
};

class CheckBox : public Widget {
public:
    CheckBox(const char* id, const char* label);
    Signal<void(bool)> on_toggled;
    void set_checked(bool checked);
    bool is_checked() const;
    void draw(Renderer* renderer) override;
};

class ProgressBar : public Widget {
public:
    ProgressBar(const char* id);
    void set_progress(float p);              // 0~1
    void set_color(const Color& fill, const Color& bg);
    void draw(Renderer* renderer) override;
};

class ScrollView : public Widget {
public:
    ScrollView(const char* id);
    void set_content(Widget* content);
    void scroll_to(float x, float y);
    void draw(Renderer* renderer) override;
    // 自动显示/隐藏滚动条
};

// ============================================================
// 3. 信号系统
// ============================================================
template<typename... Args>
class Signal {
public:
    using Callback = std::function<void(Args...)>;

    void connect(Callback cb);
    void disconnect();
    void emit(Args... args);
    // 自动断开：连接成员函数时，对象销毁自动断开
    template<typename T>
    void connect_member(T* obj, void (T::*method)(Args...));

private:
    std::vector<Callback> callbacks_;
};

// 使用示例：
// button->on_pressed.connect([]() { printf("clicked!\n"); });
// slider->on_value_changed.connect([](float v) { printf("val=%.2f\n", v); });

// ============================================================
// 4. 样式表（StyleSheet）
// ============================================================
// CSS 子集，运行时解析

class StyleSheet {
public:
    // 解析 CSS 字符串
    bool parse(const char* css_text);
    // 从文件加载
    bool load_from_file(const char* path);

    // 计算某个 Widget 的最终样式
    Style resolve(const Widget* widget) const;

    // 内置属性
    // background: #ff0000;
    // background-hover: #cc0000;
    // background-pressed: #990000;
    // color: #ffffff;
    // font-size: 16px;
    // border-radius: 4px;
    // border: 1px solid #333;
    // padding: 8px 12px;
    // margin: 4px;
    // opacity: 0.8;
    // font-family: "sans-serif";
    // text-align: center;
    // box-shadow: 0 2px 4px rgba(0,0,0,0.3);
};

// 选择器规则（CSS 子集）：
//   Button          → 标签选择器
//   #myButton       → ID 选择器
//   .btn-primary    → 类选择器
//   Button:hover    → 伪类
//   Panel > Button  → 子选择器
//   Panel Button    → 后代选择器

// 示例样式表：
// Button {
//     background: #3a3a3a;
//     color: #ffffff;
//     border-radius: 4px;
//     padding: 8px 16px;
// }
// Button:hover {
//     background: #4a4a4a;
// }
// Button:pressed {
//     background: #2a2a2a;
// }

// ============================================================
// 5. 动画系统
// ============================================================
class Animation {
public:
    // 创建动画
    static Animation* create(Widget* target, float duration,
                             AnimationType type, Easing easing = Easing::Linear);

    // 具体动画类型
    static Animation* fade_in(Widget* target, float duration);
    static Animation* fade_out(Widget* target, float duration);
    static Animation* slide_in(Widget* target, float duration, Direction dir);
    static Animation* slide_out(Widget* target, float duration, Direction dir);
    static Animation* scale(Widget* target, float duration,
                            float from, float to);
    static Animation* color_transition(Widget* target, float duration,
                                       const Color& from, const Color& to,
                                       const char* property = "background");

    // 控制
    void play();
    void pause();
    void stop();
    void set_loop(bool loop);
    void set_reverse(bool reverse);
    bool is_playing() const;

    // 链式调用
    Animation* then(Animation* next);         // 顺序播放
    Animation* parallel(Animation* together); // 并行播放

    // 回调
    Signal<void()> on_finished;

    // 框架内部调用
    void update(float dt);
};

// 使用示例：
// auto* btn = new Button("btn", "Click Me");
// Animation::fade_in(btn, 0.3f)->play();
// Animation::slide_in(btn, 0.5f, Direction::FromBottom)
//     ->then(Animation::fade_in(btn, 0.2f))
//     ->play();

// ============================================================
// 6. UI 管理器（UI Manager）
// ============================================================
class UIManager {
public:
    static UIManager* create(Renderer* renderer);
    void destroy();

    // 根节点
    void set_root(Widget* root);
    Widget* root() const;

    // 每帧调用
    void update(float dt);
    void render();  // 在 Renderer::end_frame() 前调用

    // 输入事件转发
    void on_mouse_move(float x, float y);
    void on_mouse_button(int button, bool down);
    void on_keyboard(int key, bool down);
    void on_text_input(const char* utf8);

    // 样式表
    void set_global_style(StyleSheet* style);
    StyleSheet* global_style() const;

    // 焦点管理
    Widget* focused_widget() const;
    void set_focus(Widget* widget);

    // 模态
    void push_modal(Widget* modal);
    void pop_modal();
};

} // namespace GryceEngineUtils::ui
```

---

## 五、ECS API

```cpp
// GryceEngineUtils/ecs/world.h
namespace GryceEngineUtils::ecs {

class World {
public:
    static World* create(Renderer* renderer);  // 注入外部 Renderer
    void destroy();
    void update(float dt);

    Entity* create_entity(const char* name);
    void    destroy_entity(Entity* entity);
    Scene*  scene();
};

} // namespace GryceEngineUtils::ecs
```

---

## 六、目录结构变换

### 改造前

```
Gryce-Engine/
├── core/                     ← 所有引擎代码
│   ├── render/               ← 渲染核心
│   ├── ecs/                  ← ECS
│   ├── scene/                ← 场景管理
│   ├── components/           ← 组件
│   ├── systems/              ← 系统
│   ├── platform/             ← 窗口/输入
│   ├── physics/              ← 物理引擎
│   ├── audio/                ← 音频
│   ├── script/               ← Lua 脚本
│   ├── math/                 ← 数学库
│   └── utils/                ← 工具库
├── editor/                   ← 编辑器（已删除）
├── examples/                 ← 示例
│   ├── 3DTest/
│   └── common/               ← 共享代码
│       └── shaders/          ← shader 文件
├── third_party/
├── CMakeLists.txt
└── build.py                  ← Linux 构建脚本
```

### 改造后

```
Gryce-Engine/
├── include/GryceEngineUtils/ ← 公共头文件（新增）
│   ├── renderer.h
│   ├── math.h
│   ├── types.h
│   ├── physics.h
│   ├── audio.h
│   ├── ecs/
│   │   ├── world.h
│   │   ├── scene.h
│   │   ├── entity.h
│   │   └── component.h
│   └── ui/                    ← UI 模块（新增）
│       ├── ui.h
│       ├── widget.h
│       ├── signal.h
│       ├── stylesheet.h
│       ├── animation.h
│       ├── label.h
│       ├── button.h
│       ├── panel.h
│       ├── image.h
│       ├── text_input.h
│       ├── slider.h
│       ├── checkbox.h
│       ├── progress_bar.h
│       └── scroll_view.h
│
├── src/                      ← 实现代码（从 core/ 移入）
│   ├── renderer/             ← 原 core/render/ + 新 Renderer 类
│   ├── ecs/                  ← 原 core/ecs/ + scene/ + components/ + systems/
│   ├── platform/             ← 原 core/platform/
│   ├── physics/              ← 原 core/physics/
│   ├── audio/                ← 原 core/audio/
│   ├── script/               ← 原 core/script/
│   └── ui/                   ← UI 实现（新增）
│
├── core/                     ← 保留兼容（可选），或删除
│
├── shaders/                  ← 原 examples/common/shaders/ 移入，公共资源
│
├── examples/
│   ├── minimal/              ← 新：裸框架 demo（10 行代码）
│   ├── ecs_demo/             ← 新：带 ECS 完整 demo
│   ├── 3DTest/               ← 保留，用新 API 重写
│   └── common/               ← 保留（部分共享代码）
│
├── third_party/              ← 不变
├── CMakeLists.txt            ← 多 target，可选模块
└── build.py                  ← 更新
```

---

## 七、CMake 构建目标

```cmake
# 核心渲染器库（必选）
target: gryce_renderer_static
  src: src/renderer/**

# 平台库（必选，window/input）
target: gryce_platform_static
  src: src/platform/**

# 数学库（必选，header-only 或静态）
target: gryce_math_static
  src: src/math/**

# ECS 库（可选）
option(GRYCE_ENABLE_ECS "Enable ECS module" ON)
target: gryce_ecs_static
  src: src/ecs/**
  link: gryce_renderer_static

# 物理库（可选，独立于 ECS）
option(GRYCE_ENABLE_PHYSICS "Enable Physics module" ON)
target: gryce_physics_static
  src: src/physics/**

# 音频库（可选，独立于 ECS）
option(GRYCE_ENABLE_AUDIO "Enable Audio module" ON)
target: gryce_audio_static
  src: src/audio/**

# 脚本库（可选）
option(GRYCE_ENABLE_SCRIPT "Enable Lua scripting" ON)
target: gryce_script_static
  src: src/script/**

# UI 库（可选，独立于 ECS）
option(GRYCE_ENABLE_UI "Enable UI module" ON)
target: gryce_ui_static
  src: src/ui/**
  link: gryce_renderer_static
```

---

## 八、RenderPipeline 需要加的 submit() 接口

```cpp
// RenderPipeline 新增方法（render_pipeline.h）
class RenderPipeline {
    // ... 原有 render_scene() 保留不动 ...

    // 新增：直接提交，不依赖 Scene
    void submit(IMesh* mesh, IMaterial* material,
                const math::Matrix4f& transform);
    void submit_instanced(IMesh* mesh, IMaterial* material,
                          const math::Matrix4f* transforms, int count);
    void set_submit_camera(const math::Vector3f& pos,
                           const math::Matrix4f& view_proj);
    void set_submit_lights(const LightData* lights, int count);
    void set_submit_ambient(const math::Vector3f& color);

    // Renderer 内部调用
    void render_submitted();  // 渲染所有 submit 的物体
};
```

### 实现逻辑

```
submit(mesh, material, transform)
  ├─ 将 mesh/material/transform 存入内部队列
  └─ 不做立即绘制

render_submitted()
  ├─ 遍历队列
  │   ├─ 收集到场景的渲染列表（与 render_scene 相同的路径）
  │   └─ 应用所有后处理效果
  ├─ 清空队列
  └─ 交换缓冲

render_scene() 和 render_submitted() 二选一
  └─ 用 flag 区分当前走哪个路径
```

---

## 九、World 解耦

```cpp
// 改造前
class World {
    RenderPipeline* pipeline_;  // World 内部创建
    Scene* scene_;
    // ...
};

// 改造后
class World {
    Renderer* renderer_;  // 外部注入，不管理生命周期
    Scene* scene_;
    // ...
};
```

`World::update()` 内部不再调用 `Pipeline::render_scene()`，而是：
1. 遍历 Entity
2. 收集可见物体的 mesh/material/transform
3. 调用 `renderer->draw(mesh, material, transform)`

如果用户选择了裸框架模式（不用 ECS），则 `World` 完全不参与，用户直接 `renderer->draw()`。

---

## 十、完整改造步骤

| # | 步骤 | 文件 | 说明 |
|---|------|------|------|
| 1 | 新建 `include/GryceEngineUtils/` | 8+ 个公共头文件 | re-export + 新 API 声明 |
| 2 | 新建 `Renderer` 类实现 | `src/renderer/renderer.cpp` | 封装 Pipeline + Context + 窗口 |
| 3 | RenderPipeline 加 submit 接口 | `render_pipeline.h/cpp` | 不依赖 Scene 的绘制路径 |
| 4 | 新建 `src/` 目录 | 目录结构重组 | 从 core/ 复制/移动 |
| 5 | 解耦 World | `ecs/world.h/cpp` | 注入外部 Renderer* |
| 6 | 新建 physics/audio 头文件 | `include/GryceEngineUtils/physics.h` | 独立模块入口 |
| 7 | 实现 UI 模块 | `include/GryceEngineUtils/ui/*` + `src/ui/*` | Widget 树 + 信号 + 样式表 + 动画（已完成） |
| 8 | 更新 CMakeLists.txt | 根目录 | 多 target + 可选模块 |
| 9 | 新建 minimal 示例 | `examples/minimal/main.cpp` | 裸框架 demo |
| 10 | 重写 3DTest | `examples/3DTest/3dtest.cpp` | 用新 API |
| 11 | 新建 UI 示例 | `examples/ui_demo/main.cpp` | 完整 UI 演示（已完成） |
| 12 | 更新 build.py | 根目录 | Linux 兼容（已完成，含 `--no-ui`） |

---

## 十一、四种使用场景

### 场景 A：裸框架（只要渲染）

```cpp
#include <GryceEngineUtils/renderer.h>

int main() {
    auto* r = GryceEngineUtils::Renderer::create({
        .title = "My Game",
        .width = 1280, .height = 720
    });
    auto* mesh = r->load_mesh("res:/models/cube.obj");
    auto* mat  = r->create_material();
    mat->set_albedo({1, 0.2, 0.2});

    while (r->is_running()) {
        r->begin_frame();
        r->draw(mesh, mat, math::Matrix4f::identity());
        r->end_frame();
    }
    r->destroy();
}
```

### 场景 B：裸框架 + UI

```cpp
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ui/ui.h>

int main() {
    auto* r = GryceEngineUtils::Renderer::create({...});
    auto* ui = GryceEngineUtils::ui::UIManager::create(r);

    // 创建 UI 控件
    auto* btn = new GryceEngineUtils::ui::Button("btn", "Play");
    btn->set_position(100, 100);
    btn->set_size(120, 40);
    btn->on_pressed.connect([]() { printf("Play!\n"); });

    auto* root = new GryceEngineUtils::ui::Panel("root");
    root->add_child(btn);
    ui->set_root(root);

    // 样式表
    auto* ss = new GryceEngineUtils::ui::StyleSheet();
    ss->parse("Button { background: #3a3a3a; color: #fff; border-radius: 4px; }");
    ui->set_global_style(ss);

    // 动画
    GryceEngineUtils::ui::Animation::fade_in(btn, 0.5f)->play();

    while (r->is_running()) {
        float dt = r->delta_time();
        r->begin_frame();
        // 你的 3D 渲染
        r->draw(mesh, mat, math::Matrix4f::identity());
        // UI 层
        ui->update(dt);
        ui->render();
        r->end_frame();
    }
}
```

### 场景 C：带 ECS

```cpp
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ecs/world.h>

int main() {
    auto* r = GryceEngineUtils::Renderer::create({...});
    auto* w = GryceEngineUtils::ecs::World::create(r);

    auto* e = w->create_entity("Player");
    e->add_component<MeshRenderer>("cube.obj");
    e->add_component<Transform>();

    while (r->is_running()) {
        r->begin_frame();
        w->update(r->delta_time());
        r->end_frame();
    }
}
```

### 场景 D：裸框架 + 物理

```cpp
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/physics.h>

int main() {
    auto* r = GryceEngineUtils::Renderer::create({...});
    auto* physics = GryceEngineUtils::PhysicsWorld::create();

    while (r->is_running()) {
        float dt = r->delta_time();
        physics->step(dt);
        r->begin_frame();
        // 你自己的更新和渲染
        my_update(dt);
        for (auto& obj : my_objects) {
            r->draw(obj.mesh, obj.mat, obj.transform);
        }
        r->end_frame();
    }
}
```

---

## 十二、命名空间对照

| 原有命名空间 | 新命名空间 |
|-------------|-----------|
| `gryce_engine::` | `GryceEngineUtils::` |
| `gryce_engine::render::` | `GryceEngineUtils::render::`（内部保留） |
| `gryce_engine::math::` | `GryceEngineUtils::math::` |
| `gryce_engine::ecs::` | `GryceEngineUtils::ecs::` |
| `gryce_engine::scene::` | 归入 `GryceEngineUtils::ecs::` |
| `gryce_engine::components::` | 归入 `GryceEngineUtils::ecs::` |
| `gryce_engine::systems::` | 归入 `GryceEngineUtils::ecs::` |
| `gryce_engine::physics::` | `GryceEngineUtils::`（独立） |
| `gryce_engine::audio::` | `GryceEngineUtils::`（独立） |
| `gryce_engine::script::` | `GryceEngineUtils::script::`（可选） |
| **（新模块）** | `GryceEngineUtils::ui::`（独立 UI 模块） |

---

## 十三、UI 模块实现步骤

> 状态：**已全部实现**。公共头文件位于 `include/GryceEngineUtils/ui/`，
> 实现位于 `src/ui/`，CMake target 为 `gryce_ui_static`（`GRYCE_ENABLE_UI` 开关），
> 示例为 `examples/ui_demo/main.cpp`。Renderer 已集成 `set_ui_manager()`：
> `end_frame()` 在 3D 之后自动渲染 UI，`begin_frame()` 自动转发鼠标/键盘/文本输入。

| # | 任务 | 文件 | 说明 |
|---|------|------|------|
| 1 | Widget 基类 | `widget.h`, `widget.cpp` | 位置/大小/锚点/父子/可见性/命中测试/全局点击回调 ✅ |
| 2 | 信号系统 | `signal.h` | Signal 模板类（函数类型偏特化），connect/disconnect/emit ✅ |
| 3 | 样式表解析器 | `stylesheet.h`, `stylesheet.cpp` | CSS 子集解析，选择器匹配，级联计算 ✅ |
| 4 | 动画系统 | `animation.h`, `animation.cpp` | 属性动画，链式/并行，缓动函数 ✅ |
| 5 | Label 控件 | `label.h`, `label.cpp` | 文本渲染，对齐，截断 ✅ |
| 6 | Button 控件 | `button.h`, `button.cpp` | 状态管理（normal/hover/pressed），信号 ✅ |
| 7 | Panel 容器 | `panel.h`, `panel.cpp` | 布局（Vertical/Horizontal/Grid/Absolute），背景 ✅ |
| 8 | Image 控件 | `image.h`, `image.cpp` | 纹理显示，缩放模式，着色 ✅ |
| 9 | TextInput 控件 | `text_input.h`, `text_input.cpp` | 文本编辑，光标，回车提交 ✅ |
| 10 | Slider 控件 | `slider.h`, `slider.cpp` | 拖拽，范围，值变化信号 ✅ |
| 11 | CheckBox 控件 | `checkbox.h`, `checkbox.cpp` | 勾选/取消，状态切换 ✅ |
| 12 | ProgressBar 控件 | `progress_bar.h`, `progress_bar.cpp` | 进度条，百分比显示 ✅ |
| 13 | ScrollView 容器 | `scroll_view.h`, `scroll_view.cpp` | 内容裁剪，滚动条，拖拽滚动 ✅ |
| 14 | ToolBar 工具栏 | `toolbar.h`, `toolbar.cpp` | 横向按钮组 + 分隔线，add_button/add_divider ✅ |
| 15 | MenuBar 菜单栏 | `menubar.h`, `menubar.cpp` | 顶级菜单 + 下拉菜单项，点击展开/外部点击关闭 ✅ |
| 16 | Divider 分隔线 | `divider.h`, `divider.cpp` | 横/纵分隔线 ✅ |
| 17 | ComboBox 下拉框 | `combobox.h`, `combobox.cpp` | 下拉选项，选择信号，外部点击关闭 ✅ |
| 18 | RadioButton 单选 | `radiobutton.h`, `radiobutton.cpp` | 同组互斥（组注册表）✅ |
| 19 | SpinBox 数值框 | `spinbox.h`, `spinbox.cpp` | 数值编辑 + 上下按钮，范围/步进 ✅ |
| 20 | WidgetFactory | `factory.h`, `factory.cpp` | 按类型名注册/创建控件，内置类型自动注册 ✅ |
| 21 | UIManager | `ui.h`, `ui.cpp` | 管理 Widget 树，输入转发，焦点，模态，全局点击回调 ✅ |
| 22 | 渲染集成 | `Renderer::set_ui_manager()` | UI 在 end_frame 自动渲染 + 输入自动转发 ✅ |
| 23 | UI 示例 | `examples/ui_demo/main.cpp` + `examples/uitest/main.cpp` | 完整 UI 演示 + 46 项自检断言 ✅ |

### WidgetFactory 用法

```cpp
#include <GryceEngineUtils/ui/ui.h>

// 按类型名创建内置控件
auto* btn = ui::WidgetFactory::instance().create("Button");

// 注册自定义控件后同样可创建
ui::WidgetFactory::instance().register_type("MyWidget",
    []() { return new ui::Label("my", "custom"); });
auto* w = ui::WidgetFactory::instance().create("MyWidget");
```

### 弹出层命中规则

MenuBar 下拉菜单 / ComboBox 下拉列表在父控件 bounds 之外，`hit_test`
会额外覆盖弹出区域，因此点击弹出项可正常命中；点击外部任意区域时通过
`Widget::on_global_click` 全局回调自动关闭。

### 背景闪烁修复（2026-08-30）

1. `RenderPipeline::bind_point_shadow_uniforms` 缺少 `set_shader`，
   每帧以错误 program 写入 uniform → GL_INVALID_OPERATION（已修复）。
2. `GLTexture::load_from_file` DSA 路径使用非 sized 内部格式 → GL_INVALID_ENUM（已修复）。
3. `Renderer::end_frame` 在 UI-only 帧（无 3D 提交）不清除默认 framebuffer，
   半透明 UI 背景与上一帧残留混合 → 鼠标移动时背景闪烁/拖影（已修复：
   无提交时 clear；窗口 resize 时同步管线 viewport）。

---

## 十四、UI 完整使用示例

```cpp
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ui/ui.h>

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({.title = "UI Demo", .width = 1280, .height = 720});
    auto* ui = ui::UIManager::create(r);

    // 加载样式表
    auto* ss = new ui::StyleSheet();
    ss->parse(R"(
        Button {
            background: #3a3a3a;
            color: #ffffff;
            border-radius: 4px;
            padding: 8px 16px;
            font-size: 14px;
        }
        Button:hover {
            background: #4a4a4a;
        }
        Button:pressed {
            background: #2a2a2a;
        }
        Label {
            color: #cccccc;
            font-size: 16px;
        }
        #title {
            font-size: 24px;
            color: #ffffff;
        }
    )");
    ui->set_global_style(ss);

    // 根面板
    auto* root = new ui::Panel("root");
    root->set_layout(LayoutType::Vertical);
    root->set_size(1280, 720);

    // 标题
    auto* title = new ui::Label("title", "Gryce Engine UI Demo");
    title->set_style_id("title");
    root->add_child(title);

    // 按钮
    auto* btn = new ui::Button("play_btn", "PLAY");
    btn->on_pressed.connect([]() { printf("▶ Play clicked!\n"); });
    root->add_child(btn);

    // 滑块
    auto* slider = new ui::Slider("volume");
    slider->on_value_changed.connect([](float v) {
        printf("Volume: %.2f\n", v);
    });
    root->add_child(slider);

    // 复选框
    auto* cb = new ui::CheckBox("toggle", "Enable V-Sync");
    cb->on_toggled.connect([](bool checked) {
        printf("V-Sync: %s\n", checked ? "ON" : "OFF");
    });
    root->add_child(cb);

    // 进度条
    auto* pb = new ui::ProgressBar("progress");
    root->add_child(pb);

    ui->set_root(root);

    // 入口动画
    ui::Animation::slide_in(root, 0.5f, Direction::FromBottom)->play();

    float t = 0.0f;
    while (r->is_running()) {
        float dt = r->delta_time();
        t += dt;

        // 模拟进度条
        pb->set_progress(fmodf(t, 1.0f));

        r->begin_frame();
        // 3D 场景渲染...
        // r->draw(mesh, mat, transform);
        // UI 渲染（覆盖在 3D 上）
        ui->update(dt);
        ui->render();
        r->end_frame();
    }
}
```

---

## 十五、不变的部分

- 渲染管线内部逻辑（RHI、Shader、Texture、Mesh、Pass 等）**不动**
- 所有后处理效果（SSR、DOF、Motion Blur、体积雾、水、GI）**不动**
- 阴影系统（CSM、PCF、PCSS、VSM/ESM、Atlas、点阴影）**不动**
- 物理引擎内部（Jolt、Box2D）**不动**
- 音频引擎内部（miniaudio）**不动**
- Lua 脚本引擎内部 **不动**
- 资源打包格式（GPK）**不动**
- `third_party/` 依赖 **不动**

> 核心原则：**只包装，不重写**。所有现有功能都保留，只是通过 `Renderer` 类和 `GryceEngineUtils` 命名空间暴露给用户。

---

## 十六、与旧"命令通道"的关系（CommandBar / GCommand 取舍）

改造完成后，渲染入口收敛为 `Renderer::begin_frame() → draw() → end_frame()`，
用户代码直接调用 SDK 即可出画面。旧的"编辑器构造渲染命令 → 命令栏/命令通道
执行"的环节对**渲染**不再是必需，取舍如下：

| 组件 | 建议 | 说明 |
|------|------|------|
| `RenderCommandBuffer`（渲染线程命令缓冲） | **保留** | `Renderer::end_frame()` 的内部机制，SDK 使用者无感知，不暴露 |
| `GCommand` / `GCore_PushCommand`（编辑器→引擎命令通道） | 渲染路径**不再需要** | 编辑器重建时可直接调用 `Renderer` / `ecs::World`；命令通道仅在仍需"跨线程/进程安全操作 ECS 与场景"时保留 |
| `CommandStack`（编辑器撤销/重做） | **保留** | 属于编辑器功能（undo/redo），与渲染 SDK 无关 |
| 编辑器工具栏 / CommandBar（播放、暂停、运行按钮） | UI 决策 | 属于编辑器界面层，与 SDK 直调不冲突；建议只保留与编辑器会话状态相关的按钮 |

> 结论：**SDK 直调后，任何"先构造渲染命令、再经命令栏执行"的环节都可以省略。**
> 渲染由 `Renderer` 直接接管；命令通道只服务于编辑器交互（可选），不再承担渲染职责。

---

## 十七、直接调用 SDK 渲染示例（补充）

### 示例 1：程序化网格 + 旋转立方体（纯渲染，无 ECS）

```cpp
#include <GryceEngineUtils/renderer.h>

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({.title = "Cube", .width = 1280, .height = 720});
    auto* mesh = r->load_mesh("res:/models/cube.obj");
    auto* mat  = r->create_material();
    mat->set_albedo({0.9f, 0.3f, 0.2f});

    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    float t = 0.0f;
    while (r->is_running()) {
        r->begin_frame();
        t += static_cast<float>(r->delta_time());

        const math::Vector3f eye(0.0f, 2.0f, 5.0f);
        r->set_camera(eye, proj * math::Matrix4f::look_at(
            eye, math::Vector3f::zero(), math::Vector3f::up()));

        r->draw(mesh, mat, math::Matrix4f::rotate(t, math::Vector3f::up()));
        r->end_frame();
    }
    r->destroy();
}
```

### 示例 2：批量绘制（draw_instanced）

```cpp
// 64 个实例排成 8x8 网格，一帧一次调用
std::array<math::Matrix4f, 64> transforms;
for (int i = 0; i < 64; ++i) {
    transforms[i] = math::Matrix4f::translate(
        (i % 8) * 2.0f, 0.0f, (i / 8) * 2.0f);
}
// 帧循环内：
r->begin_frame();
r->draw_instanced(mesh, mat, transforms.data(), 64);
r->end_frame();
```

### 示例 3：ECS World + 物理（场景 B + C 组合）

```cpp
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ecs/world.h>
#include <GryceEngineUtils/physics.h>

#include "components/mesh_renderer.h"   // 实体挂载 MeshRenderer 组件

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({.title = "World", .width = 1280, .height = 720});
    auto* w = ecs::World::create(r);          // 注入 Renderer，update 内部直调 draw
    auto* physics = PhysicsWorld::create();   // 独立模块，不依赖 ECS

    auto* e = w->create_entity("Player");
    e->add_component<gryce_engine::components::MeshRenderer>("res:/models/cube.obj");

    while (r->is_running()) {
        r->begin_frame();
        const float dt = static_cast<float>(r->delta_time());
        physics->step(dt);   // 物理独立推进
        w->update(dt);       // 更新系统并收集可见实体 → renderer->draw
        r->end_frame();
    }
    physics->destroy();
    w->destroy();
    r->destroy();
}
```

### 示例 4：帧截图（离屏读回）

```cpp
// end_frame() 之后调用（阻塞等待渲染线程完成）
std::vector<uint8_t> rgba;
int w = 0, h = 0;
if (r->context()->capture_frame_rgba(rgba, w, h)) {
    // rgba 为 top-down RGBA，可直接写 BMP/PNG
}
```
