// ============================================================================
// parkourDemo/main.cpp —— 3D 跑酷 demo（Gryce Engine）
// ----------------------------------------------------------------------------
// 本文件只是"游戏宿主"：初始化 Renderer/ECS World/UIManager，搭建静态 3D 基架
// （地面/护栏/方向光/相机），加载声明式界面 parkour.uif，并载入 QuickJS 玩法
// 脚本 game.js。全部玩法逻辑（生成障碍、移动、跳跃、碰撞、计分、提示）都在
// game.js 中，经引擎桥 engine.game.* 驱动 3D 实体与 UI 控件。
//
// 命令行：--vulkan 使用 Vulkan 后端（默认 OpenGL）；--help 查看帮助。
// ============================================================================
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ecs/world.h>
#include <GryceEngineUtils/ecs/entity.h>
#include <GryceEngineUtils/math.h>
#include <GryceEngineUtils/ui/ui.h>
#include <GryceEngineUtils/ui/uif_parser.h>
#include <GryceEngineUtils/ui/uif_builder.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "components/mesh_renderer.h"
#include "components/transform.h"
#include "components/light.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

using namespace GryceEngineUtils;

namespace {

// 读取 res:/ 路径对应的磁盘内容（供脚本/界面加载）
std::string read_res(const char* path) {
    std::string abs = gryce_engine::resources::ResourcePath::resolve(path);
    std::ifstream f(abs, std::ios::binary);
    if (!f) {
        GLOG_ERROR("[parkourDemo] cannot open {} -> {}", path, abs);
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 生成一个带颜色的 PBR 立方体实体（着色/尺寸在代码里配置）
gryce_engine::components::MeshRenderer* add_cube(
    ecs::World* w, const char* name, const math::Vector3f& pos,
    const math::Vector3f& scale, const math::Vector3f& albedo,
    float roughness = 0.6f, float metallic = 0.0f) {
    auto* e = w->create_entity(name);
    e->transform()->position = pos;
    e->transform()->scale = scale;
    auto* mr = e->add_component<gryce_engine::components::MeshRenderer>("res:/models/cube_pbr.obj");
    if (mr && mr->material) {
        mr->material->set_albedo(albedo);
        mr->material->set_roughness(roughness);
        mr->material->set_metallic(metallic);
    }
    return mr;
}

} // namespace

int main(int argc, char* argv[]) {
    RenderAPI api = RenderAPI::OpenGL;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            api = RenderAPI::Vulkan;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("parkourDemo [--vulkan] [--help]\n");
            return 0;
        }
    }

    // ---- Renderer / UI / World ----
    auto* r = Renderer::create({
        .title = "Gryce Engine - Parkour Run (3D)",
        .width = 1280,
        .height = 720,
        .vsync = true,
        .hdr = true,
        .shadow_map_size = 2048,
        .api = api,
    });
    if (!r) {
        std::fprintf(stderr, "[parkourDemo] Renderer::create failed\n");
        return 1;
    }

    auto* ui = ui::UIManager::create(r);
    r->set_ui_manager(ui);

    auto* w = ecs::World::create(r);

    // ---- 静态 3D 基架（引擎宿主职责）----
    // 长条地面轨道
    add_cube(w, "Ground", math::Vector3f(0.0f, 0.0f, 0.0f),
             math::Vector3f(6.5f, 0.3f, 90.0f),
             math::Vector3f(0.16f, 0.18f, 0.20f), 1.0f, 0.0f);
    // 两侧护栏（防止视觉越界）
    add_cube(w, "RailL", math::Vector3f(-3.5f, 0.9f, 0.0f),
             math::Vector3f(0.3f, 1.9f, 90.0f),
             math::Vector3f(0.45f, 0.3f, 0.25f), 0.8f, 0.0f);
    add_cube(w, "RailR", math::Vector3f(3.5f, 0.9f, 0.0f),
             math::Vector3f(0.3f, 1.9f, 90.0f),
             math::Vector3f(0.45f, 0.3f, 0.25f), 0.8f, 0.0f);
    // 起终点装饰带
    add_cube(w, "StartLine", math::Vector3f(0.0f, 0.2f, 3.0f),
             math::Vector3f(6.6f, 0.04f, 0.4f),
             math::Vector3f(0.1f, 0.9f, 0.3f), 0.9f, 0.0f);

    // 方向光（产生阴影）+ 环境光
    auto* sun = w->create_entity("Sun");
    auto* sun_light = sun->add_component<gryce_engine::components::Light>();
    sun_light->light_type = gryce_engine::components::Light::Type::Directional;
    sun_light->direction = math::Vector3f(-0.4f, -1.0f, -0.3f);
    sun_light->intensity = 1.5f;
    r->set_ambient({0.20f, 0.22f, 0.26f});

    // ---- 相机：固定在玩家后方，向前看向跑道 ----
    const math::Vector3f eye(0.0f, 3.6f, -7.5f);
    const math::Vector3f target(0.0f, 1.1f, 7.0f);
    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(66.0f), 16.0f / 9.0f, 0.1f, 220.0f);
    const math::Matrix4f view = math::Matrix4f::look_at(eye, target, math::Vector3f::up());
    r->set_camera(eye, proj * view);

    // ---- 注入玩法桥接运行时（engine.game.* 依赖它）----
    ui::EngineBridge::set_game_runtime({w, r});

    // ---- 加载声明式界面 parkour.uif ----
    {
        ui::UIParser parser;
        auto parsed = parser.parse_file(gryce_engine::resources::ResourcePath::resolve("res:/parkour.uif"));
        if (!parsed.success) {
            GLOG_ERROR("[parkourDemo] parse parkour.uif failed: {}", parsed.error_message);
        } else {
            ui::UIWidgetBuilder builder;
            auto built = builder.build(parsed.doc);
            if (built.success && built.root) {
                ui->set_root(built.root);
                GLOG_INFO("[parkourDemo] UI built: {} widget(s)", built.widget_count);
            } else {
                GLOG_ERROR("[parkourDemo] build UI failed: {}", built.error_message);
            }
        }
    }

    // ---- 载入玩法脚本 game.js（全部游戏逻辑在 QuickJS）----
    if (auto* vm = ui::EngineBridge::vm()) {
        auto res = vm->eval(read_res("res:/game.js"), "game.js");
        if (!res.success) {
            GLOG_ERROR("[parkourDemo] game.js eval failed: {}", res.error_msg);
        }
    }

    // ---- 主循环 ----
    while (r->is_running()) {
        r->begin_frame();
        const float dt = static_cast<float>(r->delta_time());

        // 玩法逐帧驱动（game.js 里定义 onGameUpdate(dt)）
        if (auto* vm = ui::EngineBridge::vm()) {
            vm->call_function("onGameUpdate", { ui::JSValueWrapper(dt) });
        }

        w->update(dt);   // 收集并提交 3D 绘制
        r->end_frame();  // 渲染 3D + UI（已 set_ui_manager）
    }

    w->destroy();
    r->destroy();
    ui->destroy();
    std::printf("[parkourDemo] all systems nominal.\n");
    return 0;
}