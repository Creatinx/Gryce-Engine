// ===========================================================================
// engine/script/js_app_bindings.h
// 入口脚本（main.js）专用绑定：窗口 / 场景 / 实体查询 / 应用控制。
//
// 与 js_bindings.h 的分工：js_bindings 是"组件视角"的基础绑定（engine.log /
// time / input / 宿主实体），不依赖任何后端；本文件补的是"应用视角"的
// 能力——建窗口、载场景、按名字找实体、退出、截图。这些能力需要宿主
// （GryceRenderer / GrycePlatform）参与，因此通过 AppHostServices 反向注入，
// 绑定层本身不链接渲染器。
//
// 脚本侧 API 面：
//   engine.app.quit() / fps() / has_window() / has_renderer()
//   engine.window.create({title,width,height,api,vsync,resizable})
//   engine.window.close() / valid() / size() -> [w,h]
//   engine.input.set_mouse_locked(bool)
//   engine.scene.load(path) -> bool / current() -> string
//   engine.entity.count() / find(name) / all() / spawn(json) / destroy(e)
//   实体代理对象（find / all / spawn 返回）：
//     id() / valid() / name() / set_name(s)
//     position() / set_position(...) / rotation() / set_rotation(...)
//     scale() / set_scale(...) / translate(dx,dy,dz)
//     enabled() / set_enabled(b) / destroy()
// ===========================================================================

#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

#include "export.h"

namespace gryce_engine::script {

class ScriptApplication;

// 窗口创建参数（main.js -> 宿主）
struct WindowOptions {
    std::string title = "Gryce";
    int width = 1280;
    int height = 720;
    std::string api;  // opengl / vulkan / dx12 / webgl；空 = 跟随 project.data
    bool vsync = true;
    bool resizable = true;
};

// 宿主提供给绑定层的服务。实现方在 app_host.cpp（链接渲染器与平台）。
struct AppHostServices {
    virtual ~AppHostServices() = default;

    virtual bool create_window(const WindowOptions& opts) = 0;
    virtual void destroy_window() = 0;
    virtual bool window_valid() const = 0;
    virtual void window_size(int* out_w, int* out_h) = 0;
    virtual void set_cursor_locked(bool locked) = 0;

    virtual bool load_scene(const std::string& path) = 0;
    virtual std::string current_scene() = 0;
    /// project.data "main_scene"：启动场景的唯一真源。脚本用
    /// engine.scene.load() 不传参时载入的就是它，从而让改配置真的换场景。
    virtual std::string main_scene() = 0;

    virtual void request_quit() = 0;
    virtual bool has_renderer() const = 0;
    virtual float fps() = 0;
    virtual bool screenshot(const std::string& path) = 0;

    // 渲染效果开关（DEMO / 编辑器运行时对比反射方案）。宿主实现直接调
    // GRender_*（app_host 链接渲染器），绑定层本身不碰渲染器符号。
    virtual void set_rt_reflection(bool enabled) = 0;
    virtual bool rt_reflection_enabled() const = 0;
    virtual void set_ssr(bool enabled) = 0;
    virtual bool ssr_enabled() const = 0;
};

// 在已安装基础绑定的 context 上追加应用绑定（幂等：每次新建 context 调用一次）。
GRYCE_API void install_app_bindings(JSContext* ctx, ScriptApplication* app,
                                    AppHostServices* services);

} // namespace gryce_engine::script
