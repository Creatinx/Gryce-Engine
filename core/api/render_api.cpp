#include "GryceRenderer/render_api.h"
#include "GryceRenderer/viewport_api.h"
#include "GryceCore/core_api.h"
#include "GrycePlatform/window_api.h"

#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/render.h"
#include "assets/asset_manager.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "ecs/world.h"
#include "ecs/query.h"
#include "math/camera.h"
#include "math/math.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using gryce_engine::render::RenderContext;
using gryce_engine::render::RenderPipeline;
using gryce_engine::render::IRenderBackend;
using gryce_engine::render::create_render_backend;
using gryce_engine::render::RenderAPI;
using gryce_engine::ecs::World;
// 让 scene/components/math 等嵌套命名空间名在本文档中可见；
// Camera 存在 math::Camera 与 components::Camera 两个类型，调用处显式限定。
using namespace gryce_engine;

namespace {

constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;

struct RendererState {
    bool initialized = false;
    bool sync_mode = false;

    std::unique_ptr<RenderContext> ctx;
    std::unique_ptr<RenderPipeline> pipeline;

    int viewport_w = 1280;
    int viewport_h = 720;
    int gameview_w = 1280;
    int gameview_h = 720;
    GEntityHandle viewport_camera = 0;
    GEntityHandle gameview_camera = 0;
    std::string display_mode = "Shaded";

    std::mutex mutex;
};

static RendererState g_renderer;

static RenderAPI to_internal_api(GRenderAPI api) {
    switch (api) {
        case GRYCE_RENDER_API_VULKAN: return RenderAPI::Vulkan;
        case GRYCE_RENDER_API_DX11:
        case GRYCE_RENDER_API_DX12:
        default: return RenderAPI::OpenGL;
    }
}

static World* get_world() {
    void* ptr = GCore_GetInternalWorldPtr();
    return static_cast<World*>(ptr);
}

// 在场景中查找主摄像机：优先 is_main，其次名字为 MainCamera，最后任意启用的摄像机。
static scene::Entity* find_main_camera_entity(scene::Scene& scn) {
    scene::Entity* result = nullptr;
    scene::Entity* fallback_by_name = nullptr;
    scene::Entity* any_enabled = nullptr;
    scn.foreach([&](scene::Entity* entity) {
        if (!entity) return;
        auto* cam = entity->get_component<components::Camera>();
        if (!cam || !cam->enabled) return;
        if (!any_enabled) any_enabled = entity;
        if (!fallback_by_name && entity->name() == "MainCamera") fallback_by_name = entity;
        if (cam->is_main && !result) result = entity;
    });
    if (result) return result;
    if (fallback_by_name) return fallback_by_name;
    return any_enabled;
}

// 用实体 Transform + Camera 组件构造渲染管线所需的 math::Camera。
static bool build_scene_camera(scene::Entity* entity, int viewport_w, int viewport_h,
                               math::Camera& out) {
    if (!entity) return false;
    auto* cam = entity->get_component<components::Camera>();
    if (!cam || !cam->enabled) return false;
    auto* t = entity->transform();
    if (!t) return false;

    // 摄像机默认看向 -Z；用实体旋转把该方向变换到世界空间，再反解 yaw/pitch。
    math::Vector3f fwd = t->rotation.rotate_vector(math::Vector3f(0.0f, 0.0f, -1.0f));
    if (fwd.length_sq() < 1e-6f) fwd = math::Vector3f(0.0f, 0.0f, -1.0f);
    fwd = fwd.normalized();

    const float pitch = std::asin(math::clamp(fwd.y, -1.0f, 1.0f));
    const float yaw = std::atan2(fwd.z, fwd.x);

    out.set_position(t->position);
    out.set_yaw(yaw * kRadToDeg);
    out.set_pitch(pitch * kRadToDeg);
    out.set_fov(cam->fov);
    out.set_near_far(cam->near_plane, cam->far_plane);
    out.set_aspect(viewport_h > 0
                       ? static_cast<float>(viewport_w) / static_cast<float>(viewport_h)
                       : 16.0f / 9.0f);
    return true;
}

// 收集场景中的全部光源（最多 8 盏），供 PBR 多光源渲染使用。
static void collect_scene_lights(scene::Scene& scn,
                                 std::vector<RenderPipeline::Light>& out) {
    out.clear();
    scn.foreach([&](scene::Entity* entity) {
        if (!entity || out.size() >= RenderPipeline::k_max_lights) return;
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;
        RenderPipeline::Light l;
        l.type = static_cast<RenderPipeline::LightType>(light->light_type);
        l.direction = light->direction;
        l.color = light->color;
        l.intensity = light->intensity;
        l.range = light->range;
        l.spot_angle = light->spot_angle;
        l.spot_softness = light->spot_softness;
        auto* t = entity->transform();
        l.position = t ? t->position : math::Vector3f::zero();
        out.push_back(l);
    });
}

// 同步模式下渲染线程未运行，RenderSystem3D 的上传路径不会执行；
// 这里在绘制前把尚未上传 GPU 的 MeshRenderer / SkinnedMeshRenderer 补传上去。
static void upload_pending_meshes(scene::Scene& scn, RenderContext& ctx) {
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::MeshRenderer* mr, components::Transform*) {
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            auto data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (data && !data->empty()) {
                mr->upload_to_gpu(&ctx, data.get(), /*allow_while_running=*/false);
            }
        });

    ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::SkinnedMeshRenderer* mr, components::Transform*) {
            if (!mr || !mr->enabled || mr->model_path.empty() || mr->gpu_mesh()) return;
            mr->upload_to_gpu(&ctx, /*allow_while_running=*/false);
        });

    // 编辑器修改材质贴图路径/use 标志后，material 被标记 textures_dirty；
    // 在这里统一重新 upload，让改动下一帧生效。
    auto refresh_dirty_material = [&](render::Material* mat) {
        if (mat && mat->textures_dirty) {
            mat->upload_to_gpu(&ctx);
            mat->textures_dirty = false;
        }
    };
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::MeshRenderer* mr, components::Transform*) {
            if (mr) refresh_dirty_material(mr->material.get());
        });
    ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::SkinnedMeshRenderer* mr, components::Transform*) {
            if (mr) refresh_dirty_material(mr->material.get());
        });
}

} // namespace

extern "C" {

int GRender_Init(const GRenderInitDesc* desc) {
    if (!desc || desc->version != sizeof(GRenderInitDesc)) return -1;

    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.initialized) return 0;

    g_renderer.sync_mode = desc->sync_mode;

    auto backend = create_render_backend(to_internal_api(desc->api));
    if (!backend) {
        GLOG_ERROR("GRender_Init: failed to create render backend");
        return -1;
    }

    // 优先使用平台层提供的渲染句柄（External HWND 模式下为嵌入的 GLFW 窗口）
    GWindowHandle render_handle = desc->native_window;
    if (GWindow_IsValid()) {
        GWindowHandle platform_render = GWindow_GetRenderHandle();
        if (platform_render) render_handle = platform_render;
    }

    g_renderer.ctx = std::make_unique<RenderContext>();
    if (!g_renderer.ctx->init(render_handle, std::move(backend))) {
        GLOG_ERROR("GRender_Init: RenderContext::init failed");
        g_renderer.ctx.reset();
        return -1;
    }

    g_renderer.viewport_w = desc->viewport_w > 0 ? desc->viewport_w : 1280;
    g_renderer.viewport_h = desc->viewport_h > 0 ? desc->viewport_h : 720;
    g_renderer.gameview_w = g_renderer.viewport_w;
    g_renderer.gameview_h = g_renderer.viewport_h;

    // Init render pipeline for 3D scene rendering
    g_renderer.pipeline = std::make_unique<RenderPipeline>();
    // WPF 编辑器通过嵌入的 GLFW HWND 直接显示场景（ViewportHwndHost），
    // 因此 tonemap 必须写入默认帧缓冲（交换链），而不是离屏 viewport FBO。
    // 离屏输出留给需要采样纹理的宿主（如旧 ImGui 编辑器）使用。
    g_renderer.pipeline->set_viewport_output_enabled(false);
    if (!g_renderer.pipeline->init(g_renderer.ctx.get(), "res:/shaders")) {
        GLOG_WARN("GRender_Init: RenderPipeline init failed (shaders may be missing), falling back to clear-only");
        g_renderer.pipeline.reset();
    }

    // Async mode: start render thread
    if (!g_renderer.sync_mode) {
        g_renderer.ctx->start();
    }

    g_renderer.initialized = true;
    GLOG_INFO("GRender_Init: {} mode, {}x{}",
              g_renderer.sync_mode ? "sync" : "async",
              g_renderer.viewport_w, g_renderer.viewport_h);
    return 0;
}

void GRender_Shutdown(void) {
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.initialized) return;

    if (g_renderer.pipeline) {
        g_renderer.pipeline->shutdown();
        g_renderer.pipeline.reset();
    }

    if (g_renderer.ctx) {
        g_renderer.ctx->shutdown();
        g_renderer.ctx.reset();
    }

    g_renderer.initialized = false;
    g_renderer.sync_mode = false;
    GLOG_INFO("GRender_Shutdown: renderer destroyed");
}

bool GRender_IsInitialized(void) {
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.initialized;
}

void GRender_BeginFrame(void) {
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;

    if (g_renderer.sync_mode) {
        auto* backend = g_renderer.ctx->backend();
        if (backend) backend->begin_frame();
    }
    // Async mode: render thread handles begin_frame
}

// Renders the current world through the pipeline (shared by SceneView / GameView).
static void render_world_internal() {
    if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;

    auto* world = get_world();
    if (!world || !world->scene()) {
        // No world yet — just clear to dark gray
        if (g_renderer.sync_mode) {
            auto* backend = g_renderer.ctx->backend();
            if (backend) backend->clear(0.15f, 0.15f, 0.15f, 1.0f);
        } else {
            g_renderer.ctx->clear(0.15f, 0.15f, 0.15f, 1.0f);
        }
        return;
    }

    // Set viewport
    if (g_renderer.sync_mode) {
        auto* backend = g_renderer.ctx->backend();
        if (backend) backend->set_viewport(0, 0, g_renderer.viewport_w, g_renderer.viewport_h);
    } else {
        g_renderer.ctx->set_viewport(0, 0, g_renderer.viewport_w, g_renderer.viewport_h);
    }

    // Render via pipeline if available
    if (g_renderer.pipeline && g_renderer.pipeline->is_valid()) {
        // 同步模式：先补传网格，再解析摄像机/光源，最后渲染。
        if (g_renderer.sync_mode) {
            upload_pending_meshes(*world->scene(), *g_renderer.ctx);
        }
        g_renderer.pipeline->set_viewport(g_renderer.viewport_w, g_renderer.viewport_h);
        // 从场景中解析主摄像机与光源，喂给渲染管线（此前未设置 camera_，
        // render_scene 直接 return，场景始终画不出来）。
        math::Camera camera;
        if (build_scene_camera(
                find_main_camera_entity(*world->scene()),
                g_renderer.viewport_w, g_renderer.viewport_h, camera)) {
            g_renderer.pipeline->set_camera(camera);
        }
        std::vector<RenderPipeline::Light> lights;
        collect_scene_lights(*world->scene(), lights);
        g_renderer.pipeline->set_lights(lights);
        g_renderer.pipeline->render_scene(*world->scene(), *g_renderer.ctx);
    } else {
        // No pipeline — fallback: let world render systems push commands
        world->render(*g_renderer.ctx);
    }

}

void GRender_RenderWorld(void) {
    std::lock_guard lock(g_renderer.mutex);
    render_world_internal();
}

void GRender_RenderGizmo(void) {
    // TODO(Phase 4): ImGuizmo + Viewport Toolbar
}

void GRender_RenderGameView(void) {
    std::lock_guard lock(g_renderer.mutex);
    // TODO(Phase): 独立 GameView FBO 与独立相机。目前与 SceneView 共用同一管线/纹理，
    // 与 GRender_GetGameViewTexture() 返回视口纹理的行为保持一致。
    render_world_internal();
}

void GRender_SetDisplayMode(const char* mode) {
    std::lock_guard lock(g_renderer.mutex);
    if (!mode || mode[0] == '\0') return;
    g_renderer.display_mode = mode;
    GLOG_INFO("GRender_SetDisplayMode: {}", g_renderer.display_mode);
    // TODO(Phase): 在下层后端/管线应用线框模式（当前仅记录，UI 已正确接线）。
}

void GRender_EndFrame(void) {
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;

    if (g_renderer.sync_mode) {
        g_renderer.ctx->present_sync();
    } else {
        g_renderer.ctx->present();
    }
}

GTextureHandle GRender_GetViewportTexture(void) {
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.pipeline || !g_renderer.pipeline->is_valid()) return nullptr;
    auto* tex = g_renderer.pipeline->viewport_color_texture();
    return static_cast<GTextureHandle>(tex);
}

GTextureHandle GRender_GetGameViewTexture(void) {
    // TODO: separate GameView FBO when PlayMode is active
    return GRender_GetViewportTexture();
}

int GRender_GetViewportSize(int* out_w, int* out_h) {
    if (!out_w || !out_h) return -1;
    std::lock_guard lock(g_renderer.mutex);
    *out_w = g_renderer.viewport_w;
    *out_h = g_renderer.viewport_h;
    return 0;
}

int GRender_GetGameViewSize(int* out_w, int* out_h) {
    if (!out_w || !out_h) return -1;
    std::lock_guard lock(g_renderer.mutex);
    *out_w = g_renderer.gameview_w;
    *out_h = g_renderer.gameview_h;
    return 0;
}

void GRender_SetVSync(bool enabled) {
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.ctx) return;
    g_renderer.ctx->set_swap_interval(enabled ? 1 : 0);
}

// ========== Viewport API ==========

void GViewport_SetSize(int w, int h) {
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.viewport_w = w > 0 ? w : 1;
    g_renderer.viewport_h = h > 0 ? h : 1;
    if (g_renderer.pipeline && g_renderer.pipeline->is_valid()) {
        g_renderer.pipeline->resize_render_targets(g_renderer.viewport_w, g_renderer.viewport_h);
    }
}

void GViewport_GetSize(int* out_w, int* out_h) {
    if (out_w) *out_w = g_renderer.viewport_w;
    if (out_h) *out_h = g_renderer.viewport_h;
}

void GViewport_SetCamera(GEntityHandle camera_entity) {
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.viewport_camera = camera_entity;
}

GEntityHandle GViewport_GetCamera(void) {
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.viewport_camera;
}

void GGameView_SetSize(int w, int h) {
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.gameview_w = w > 0 ? w : 1;
    g_renderer.gameview_h = h > 0 ? h : 1;
}

void GGameView_GetSize(int* out_w, int* out_h) {
    if (out_w) *out_w = g_renderer.gameview_w;
    if (out_h) *out_h = g_renderer.gameview_h;
}

void GGameView_SetCamera(GEntityHandle camera_entity) {
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.gameview_camera = camera_entity;
}

} // extern "C"
