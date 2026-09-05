// 3DTest — 综合 3D demo（用新 GryceEngineUtils API 重写）
//
// 组合场景 A + B：Renderer 外观（窗口/上下文/管线）+ ECS World
// （外部注入 Renderer*，update 内部经 renderer->draw() 提交绘制）。
//
// 命令行：
//   --vulkan  使用 Vulkan 后端（默认 OpenGL）
//   --help    查看参数
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ecs/world.h>
#include <GryceEngineUtils/ecs/entity.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "components/mesh_renderer.h"
#include "components/transform.h"
#include "components/light.h"

using namespace GryceEngineUtils;

namespace {

gryce_engine::components::MeshRenderer* add_cube(
    ecs::World* w, const char* name, const math::Vector3f& pos,
    const math::Vector3f& scale, const math::Vector3f& albedo,
    float roughness = 0.5f, float metallic = 0.0f) {
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
            std::printf("3dtest [--vulkan] [--help]\n");
            return 0;
        }
    }

    auto* r = Renderer::create({
        .title = "Gryce Engine - 3D Test (GryceEngineUtils)",
        .width = 1280,
        .height = 720,
        .vsync = true,
        .hdr = true,
        .shadow_map_size = 2048,
        .api = api,
    });
    if (!r) {
        std::fprintf(stderr, "3dtest: Renderer::create failed\n");
        return 1;
    }

    auto* w = ecs::World::create(r);

    // 地面 + 若干立方体（MeshRenderer + Transform，由 World 收集绘制）
    add_cube(w, "Ground", math::Vector3f::zero(),
             math::Vector3f(12.0f, 0.2f, 12.0f),
             math::Vector3f(0.35f, 0.35f, 0.38f), 0.9f, 0.0f);
    auto* cube1 = add_cube(w, "Cube1", math::Vector3f(0.0f, 1.2f, 0.0f),
                           math::Vector3f::one(),
                           math::Vector3f(0.95f, 0.25f, 0.2f), 0.35f, 0.05f);
    auto* cube2 = add_cube(w, "Cube2", math::Vector3f(2.2f, 0.6f, 1.4f),
                           math::Vector3f(0.8f, 0.8f, 0.8f),
                           math::Vector3f(0.2f, 0.65f, 0.95f), 0.25f, 0.6f);
    auto* cube3 = add_cube(w, "Cube3", math::Vector3f(-2.0f, 0.5f, -1.2f),
                           math::Vector3f(0.6f, 0.6f, 0.6f),
                           math::Vector3f(0.9f, 0.8f, 0.2f), 0.5f, 0.0f);

    // 光源：方向光（阴影）+ 点光
    auto* sun = w->create_entity("Sun");
    auto* sun_light = sun->add_component<gryce_engine::components::Light>();
    sun_light->light_type = gryce_engine::components::Light::Type::Directional;
    sun_light->direction = math::Vector3f(-0.5f, -1.0f, -0.3f);
    sun_light->intensity = 1.2f;

    auto* point = w->create_entity("PointLight");
    point->transform()->position = math::Vector3f(1.0f, 3.0f, 1.0f);
    auto* point_light = point->add_component<gryce_engine::components::Light>();
    point_light->light_type = gryce_engine::components::Light::Type::Point;
    point_light->color = math::Vector3f(1.0f, 0.8f, 0.6f);
    point_light->intensity = 6.0f;
    point_light->range = 12.0f;

    // 相机（自动环绕场景）
    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    float angle = 0.0f;
    const auto update_camera = [&](Renderer* renderer, float a) {
        const math::Vector3f eye(std::cos(a) * 7.0f, 4.5f, std::sin(a) * 7.0f);
        const math::Matrix4f view = math::Matrix4f::look_at(
            eye, math::Vector3f(0.0f, 1.0f, 0.0f), math::Vector3f::up());
        renderer->set_camera(eye, proj * view);
    };
    update_camera(r, 0.0f);

    while (r->is_running()) {
        r->begin_frame();
        const float dt = static_cast<float>(r->delta_time());
        angle += dt * 0.35f;

        // 旋转立方体
        if (cube1) cube1->owner()->transform()->rotation =
            math::Quaternionf::from_axis_angle(math::Vector3f::up(), angle);
        if (cube2) cube2->owner()->transform()->rotation =
            math::Quaternionf::from_axis_angle(math::Vector3f(1.0f, 0.0f, 0.0f), angle * 0.7f);
        if (cube3) cube3->owner()->transform()->rotation =
            math::Quaternionf::from_axis_angle(math::Vector3f(0.0f, 0.0f, 1.0f), angle * 0.5f);

        update_camera(r, angle * 0.2f);
        w->update(dt);
        r->end_frame();
    }

    w->destroy();
    r->destroy();
    std::printf("3dtest: all systems nominal.\n");
    return 0;
}
