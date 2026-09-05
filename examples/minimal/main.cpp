// minimal — 裸框架 demo（场景 A：只要渲染，10 行核心循环）
//
// 不依赖 Scene/Entity/ECS：直接创建 Renderer，加载网格/材质，
// 在 while 循环里 renderer->draw() 提交绘制。
#include <GryceEngineUtils/renderer.h>

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({
        .title = "Gryce Minimal",
        .width = 1280,
        .height = 720,
        .api = RenderAPI::OpenGL,
    });
    if (!r) return 1;

    // 相机：俯瞰原点（view_proj = proj * view）
    const math::Vector3f eye(0.0f, 2.0f, 5.0f);
    const math::Matrix4f view = math::Matrix4f::look_at(
        eye, math::Vector3f::zero(), math::Vector3f::up());
    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    r->set_camera(eye, proj * view);

    // 方向光（开阴影）
    LightData light;
    light.type = render::LightType::Directional;
    light.direction = math::Vector3f(-0.5f, -1.0f, -0.3f);
    light.color = math::Vector3f::one();
    light.intensity = 1.0f;
    r->set_lights(&light, 1);

    // 一个三角形（每顶点 8 个 float：position(3) + normal(3) + uv(2)）
    const float verts[] = {
         0.0f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   0.5f, 0.0f,
        -0.5f, -0.4f, 0.0f,   0.0f, 0.0f, 1.0f,   0.0f, 1.0f,
         0.5f, -0.4f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f,
    };
    const uint32_t indices[] = {0, 1, 2};
    auto* mesh = r->create_mesh(verts, 3, indices, 3);
    auto* mat = r->create_material();
    mat->set_albedo({1.0f, 0.2f, 0.2f});

    while (r->is_running()) {
        r->begin_frame();
        r->draw(mesh, mat, math::Matrix4f::identity());
        r->end_frame();
    }
    r->destroy();
    return 0;
}
