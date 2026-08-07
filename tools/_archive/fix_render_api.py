import re

p = r'D:/Gryce-Engine/core/api/render_api.cpp'
with open(p, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace the entire GRender_Init function body
old_init = '''int GRender_Init(const GRenderInitDesc* desc) {
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
    g_renderer.pipeline = std::make_unique<RenderPipeline>();
    g_renderer.pipeline->set_viewport_output_enabled(true);
    if (!g_renderer.pipeline->init(g_renderer.ctx.get(), "res:/shaders")) {
        GLOG_WARN("GRender_Init: RenderPipeline init failed (shaders may be missing), falling back to clear-only");
        g_renderer.pipeline.reset();
    }
    g_renderer.pipeline = std::make_unique<RenderPipeline>();
    if (!g_renderer.pipeline->init(g_renderer.ctx.get(), "res:/shaders")) {
        GLOG_WARN("GRender_Init: RenderPipeline init failed (shaders may be missing), falling back to clear-only");
        g_renderer.pipeline.reset();
    } else {
        // Enable viewport offscreen output for Editor
        g_renderer.pipeline->set_viewport_output_enabled(true);
        if (!g_renderer.pipeline->create_viewport_target(g_renderer.ctx.get())) {
            GLOG_WARN("GRender_Init: viewport offscreen target creation failed");
        }
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
}'''

new_init = '''int GRender_Init(const GRenderInitDesc* desc) {
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
}'''

if old_init in content:
    content = content.replace(old_init, new_init)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Fixed GRender_Init')
else:
    print('Old GRender_Init not found, writing fresh file...')
''', file=open('/dev/null', 'w'))  # placeholder - will handle differently
