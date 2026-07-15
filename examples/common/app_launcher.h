#pragma once

#include <functional>
#include <memory>
#include <string>

#include "ecs/world.h"
#include "platform/input.h"
#include "render/render2d.h"
#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "scene/scene.h"

namespace gryce_engine::examples {

// ---------------------------------------------------------------------------
// DemoApp — 示例程序接口
// 每个 demo 只需实现场景初始化、系统注册和每帧更新。
// ---------------------------------------------------------------------------
class DemoApp {
public:
    virtual ~DemoApp() = default;

    // 窗口标题
    virtual const char* title() const = 0;

    // 是否是 3D 演示（决定使用 RenderPipeline 还是 Renderer2D）
    virtual bool is_3d() const { return false; }

    // 是否启用 ImGui
    virtual bool use_imgui() const { return true; }

    // 注册 ECS 系统；pipeline 在 3D demo 中有效，renderer2d 在 2D demo 中有效
    virtual void register_systems(ecs::World& world,
                                  render::RenderPipeline* pipeline,
                                  render::IRenderer2D* renderer2d) = 0;

    // 初始化场景内容（此时 RenderContext 仍在主线程，可上传 GPU 资源）
    virtual bool init_scene(scene::Scene& scene, render::RenderContext& ctx) = 0;

    // 每帧更新（在 ECS World::update 之前调用）
    virtual void update(float dt, platform::InputManager& input, scene::Scene& scene) = 0;

    // 每帧 ImGui UI（在主线程、world.render 之后调用）
    virtual void render_ui(scene::Scene& scene) {}
};

// ---------------------------------------------------------------------------
// 从可执行文件位置向上查找引擎仓库根目录，再进入 examples/<exe_name>
// ---------------------------------------------------------------------------
std::filesystem::path find_project_root();

// ---------------------------------------------------------------------------
// 运行一个 demo：处理窗口、渲染上下文、ImGui、主循环、清理。
// argc/argv 用于解析 --vulkan / --vulkan-validation 等命令行。
// ---------------------------------------------------------------------------
int run_demo(DemoApp& app, int argc, char* argv[]);

} // namespace gryce_engine::examples
