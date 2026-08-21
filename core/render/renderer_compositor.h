#pragma once

#include <memory>
#include <string>

#include "render/export.h"

namespace gryce_engine::render {

class RendererCanvasRender;
class RendererSceneRender;
class RenderContext;
class RenderingDevice;

// 前向声明 Storage 接口
class RendererLightStorage;
class RendererMaterialStorage;
class RendererMeshStorage;
class RendererTextureStorage;
class RendererParticlesStorage;
class RendererUtilities;

// 前向声明 Fog/GI
class RendererFog;
class RendererGI;

// ---------------------------------------------------------------------------
// RendererCompositor — 渲染器组合器
// 类似于 Godot 的 RendererCompositor，是渲染后端的顶层抽象。
// 持有 Canvas 渲染器、Scene 渲染器、Storage 系统等子组件，
// 并负责它们的生命周期管理。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererCompositor {
public:
    virtual ~RendererCompositor() = default;

    // 生命周期
    virtual bool initialize(RenderContext* ctx) = 0;
    virtual void finalize() = 0;

    // 帧生命周期
    virtual void begin_frame(double frame_step) = 0;
    virtual void end_frame(bool present) = 0;

    // 子渲染器访问
    virtual RendererCanvasRender* get_canvas() = 0;
    virtual RendererSceneRender* get_scene() = 0;
    virtual RendererFog* get_fog() = 0;
    virtual RendererGI* get_gi() = 0;

    // Storage 系统访问
    virtual RendererLightStorage* get_light_storage() = 0;
    virtual RendererMaterialStorage* get_material_storage() = 0;
    virtual RendererMeshStorage* get_mesh_storage() = 0;
    virtual RendererTextureStorage* get_texture_storage() = 0;
    virtual RendererParticlesStorage* get_particles_storage() = 0;
    virtual RendererUtilities* get_utilities() = 0;

    // RenderingDevice 访问
    virtual RenderingDevice* get_device() = 0;

    // 名称
    virtual const char* name() const = 0;

    // 单例
    static RendererCompositor* get_singleton();
    static void set_singleton(RendererCompositor* compositor);

    // 工厂函数类型
    using CreateFunc = RendererCompositor* (*)(RenderContext*);
    static void set_create_func(CreateFunc func);
    static RendererCompositor* create(RenderContext* ctx);

private:
    static RendererCompositor* singleton_;
    static CreateFunc create_func_;
};

} // namespace gryce_engine::render