#include "GryceRenderer/render_api.h"
#include "GryceRenderer/viewport_api.h"
#include "GryceCore/core_api.h"

#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/render.h"
#include "ecs/world.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <memory>
#include <mutex>

using gryce_engine::render::RenderContext;
using gryce_engine::render::RenderPipeline;
using gryce_engine::render::IRenderBackend;
using gryce_engine::render::create_render_backend;
using gryce_engine::render::RenderAPI;
using gryce_engine::ecs::World;

namespace {

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

    g_renderer.ctx = std::make_unique<RenderContext>();
    if (!g_renderer.ctx->init(desc->native_window, std::move(backend))) {
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
    g_renderer.pipeline->set_viewport_output_enabled(true);
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

void GRender_RenderWorld(void) {
    std::lock_guard lock(g_renderer.mutex);
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
        g_renderer.pipeline->set_viewport(g_renderer.viewport_w, g_renderer.viewport_h);
        g_renderer.pipeline->render_scene(*world->scene(), *g_renderer.ctx);
    } else {
        // No pipeline — fallback: let world render systems push commands
        world->render(*g_renderer.ctx);
    }
}

void GRender_RenderGizmo(void) {
    // TODO(Phase 4): ImGuizmo + Viewport Toolbar
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
