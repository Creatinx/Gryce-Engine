// ecs_demo — 带 ECS 的完整 demo（场景 B）
//
// World 不再自己创建渲染管线：外部注入 Renderer*，update() 内部
// 遍历 Entity、收集 mesh/material/transform，通过 renderer->draw() 提交。
#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ecs/world.h>
#include <GryceEngineUtils/ecs/entity.h>

#include "components/mesh_renderer.h"
#include "components/transform.h"
#include "components/light.h"

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({
        .title = "Gryce ECS Demo",
        .width = 1280,
        .height = 720,
        .api = RenderAPI::OpenGL,
    });
    if (!r) return 1;

    auto* w = ecs::World::create(r);

    // 立方体实体：MeshRenderer + Transform
    auto* cube = w->create_entity("Player");
    auto* mr = cube->add_component<gryce_engine::components::MeshRenderer>("res:/models/cube.obj");
    if (mr && mr->material) {
        mr->material->set_albedo({0.2f, 0.6f, 1.0f});
        mr->material->set_roughness(0.4f);
        mr->material->set_metallic(0.1f);
    }

    // 方向光实体
    auto* light_e = w->create_entity("Sun");
    auto* light = light_e->add_component<gryce_engine::components::Light>();
    light->light_type = gryce_engine::components::Light::Type::Directional;
    light->direction = math::Vector3f(-0.5f, -1.0f, -0.3f);
    light->intensity = 1.2f;

    // 相机
    const math::Vector3f eye(3.0f, 2.0f, 4.0f);
    const math::Matrix4f view = math::Matrix4f::look_at(
        eye, math::Vector3f::zero(), math::Vector3f::up());
    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    r->set_camera(eye, proj * view);

    while (r->is_running()) {
        r->begin_frame();
        // 旋转立方体（Transform 位置/旋转/缩放直接操作）
        cube->transform()->rotation =
            cube->transform()->rotation * math::Quaternionf::from_axis_angle(
                math::Vector3f(0.0f, 1.0f, 0.0f), math::to_radians(45.0f) * static_cast<float>(r->delta_time()));
        w->update(r->delta_time());
        r->end_frame();
    }

    w->destroy();
    r->destroy();
    return 0;
}
