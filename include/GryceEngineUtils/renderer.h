#pragma once

// GryceEngineUtils::renderer.h — 嵌入式渲染器（核心！）
//
// Renderer 封装 窗口 + RenderContext + RenderPipeline，提供不依赖
// Scene/Entity 的直接提交绘制路径：
//
//   auto* r = GryceEngineUtils::Renderer::create({...});
//   while (r->is_running()) {
//       r->begin_frame();
//       r->draw(mesh, mat, math::Matrix4f::identity());
//       r->end_frame();
//   }
//   r->destroy();

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/types.h"
#include "GryceEngineUtils/math.h"

namespace gryce_engine::platform { class Window; }
namespace gryce_engine::render { class RenderContext; class RenderPipeline; class IMesh; class IRenderer2D; }

namespace GryceEngineUtils {
namespace ui { class UIManager; }

// ---------------------------------------------------------------------------
// RendererConfig — 渲染器创建参数
// ---------------------------------------------------------------------------
struct RendererConfig {
    const char* title = "Gryce";
    int width = 1280;
    int height = 720;
    bool vsync = true;
    bool hdr = true;
    int shadow_map_size = 2048;
    RenderAPI api = RenderAPI::OpenGL;
};

// ---------------------------------------------------------------------------
// Renderer — 渲染器外观（Facade）
// 生命周期：create() 创建，destroy() 销毁；析构由 destroy 负责。
// ---------------------------------------------------------------------------
class Renderer {
public:
    static Renderer* create(const RendererConfig& config);
    void destroy();

    // ---- 帧控制 ----------------------------------------------------------
    void begin_frame();          // poll 事件 + 更新帧统计
    void end_frame();            // 渲染所有 submit 的物体 + 提交缓冲
    bool is_running();
    double delta_time() const;

    // ---- 直接提交绘制（核心！不依赖 Scene/Entity）------------------------
    void draw(IMesh* mesh, IMaterial* material,
              const math::Matrix4f& transform);
    void draw_instanced(IMesh* mesh, IMaterial* material,
                        const math::Matrix4f* transforms, int count);

    // ---- 相机与光源 ------------------------------------------------------
    void set_camera(const math::Vector3f& position,
                    const math::Matrix4f& view_proj);
    void set_lights(const LightData* lights, int count);
    void set_ambient(const math::Vector3f& color);

    // ---- 资源管理 --------------------------------------------------------
    IMesh*     load_mesh(const char* path);
    IMesh*     create_mesh(const float* vertices, int vcount,
                           const uint32_t* indices, int icount);
    IMaterial* load_material(const char* path);
    IMaterial* create_material();
    ITexture*  load_texture(const char* path);
    IShader*   load_shader(const char* vert_path, const char* frag_path);

    // ---- 效果开关 --------------------------------------------------------
    void set_shadow_enabled(bool on);
    void set_ssr_enabled(bool on);
    void set_dof_enabled(bool on);
    void set_motion_blur_enabled(bool on);
    void set_fog_enabled(bool on);
    void set_water_enabled(bool on);
    void set_gi_enabled(bool on);

    // ---- 高级访问（需要时直接操作底层）-----------------------------------
    gryce_engine::render::RenderPipeline* pipeline();
    gryce_engine::render::RenderContext*  context();
    // 2D 渲染器（UI 模块内部使用）
    gryce_engine::render::IRenderer2D* renderer2d();
    // 由本 Renderer 加载的纹理指针 → RHI 句柄（Image 控件绘制用）
    gryce_engine::render::RHITextureHandle texture_handle(const ITexture* tex) const;
    // 当前窗口尺寸（像素）
    void window_size(int& w, int& h) const;
    void* native_window();

    // ---- 键盘查询（供脚本/玩法逻辑读取）-----------------------------------
    // key 为 GLFW 键码；held=按住，pressed=本帧刚按下
    bool key_held(int key) const;
    bool key_pressed(int key) const;

    // ---- UI 集成 ----------------------------------------------------------
    // 注册 UIManager 后，end_frame() 会在 3D 渲染之后自动绘制 UI 并转发输入
    void set_ui_manager(ui::UIManager* ui);
    // 内部：GLFW 字符回调转发（UIManager::on_text_input）
    void context_ui_text_input(const char* utf8);

private:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace GryceEngineUtils
