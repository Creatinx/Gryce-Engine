#include "GryceEngineUtils/renderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <GLFW/glfw3.h>

#if defined(GRYCE_ENABLE_UI)
#include "GryceEngineUtils/ui/ui.h"
#endif
#include "platform/window.h"
#include "platform/input.h"
#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils {

namespace {

#if defined(GRYCE_ENABLE_UI)

// 字符回调需要把 codepoint 转成 UTF-8 再交给 UI
void utf8_encode(uint32_t cp, char out[5]) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        out[1] = '\0';
    } else if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = '\0';
    } else if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        out[3] = '\0';
    } else {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        out[4] = '\0';
    }
}

Renderer* g_char_callback_renderer = nullptr;

void on_char_callback(GLFWwindow*, unsigned int codepoint) {
    if (!g_char_callback_renderer) return;
    char buf[5] = {};
    utf8_encode(codepoint, buf);
    g_char_callback_renderer->context_ui_text_input(buf);
}

#endif // GRYCE_ENABLE_UI

} // namespace

namespace {

// 从可执行文件位置向上查找引擎仓库根（含 CMakeLists.txt + src/），
// 再进入 examples/<exe>/ 作为项目根，供 res:/ 路径解析使用。
std::string find_project_root() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        std::filesystem::path exe_path(buffer);
        std::filesystem::path dir = exe_path.parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (std::filesystem::exists(dir / "CMakeLists.txt") &&
                std::filesystem::is_directory(dir / "src")) {
                std::string exe_name = exe_path.stem().string();
                std::filesystem::path candidate = dir / "examples" / exe_name;
                if (std::filesystem::exists(candidate / "project.data")) {
                    return candidate.string();
                }
                return dir.string();
            }
            dir = dir.parent_path();
        }
    }
#else
    std::error_code ec;
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) {
        std::filesystem::path dir = exe_path.parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (std::filesystem::exists(dir / "CMakeLists.txt") &&
                std::filesystem::is_directory(dir / "src")) {
                std::string exe_name = exe_path.stem().string();
                std::filesystem::path candidate = dir / "examples" / exe_name;
                if (std::filesystem::exists(candidate / "project.data")) {
                    return candidate.string();
                }
                return dir.string();
            }
            dir = dir.parent_path();
        }
    }
#endif
    return std::filesystem::current_path().string();
}

// 顶点布局：position(3) + normal(3) + tangent(3) + uv(2) + color(3)
// 与 MeshVertex / 引擎内部 VertexGPU 布局一致。
void set_mesh_layout(gryce_engine::render::IMesh* mesh) {
    using namespace gryce_engine::render;
    VertexLayout layout;
    layout.stride = sizeof(gryce_engine::assets::MeshVertex);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)},
    };
    mesh->set_layout(layout);
}

bool upload_mesh_data(gryce_engine::render::IMesh* mesh,
                      const gryce_engine::assets::MeshData& data) {
    if (!mesh || data.vertices.empty()) return false;
    mesh->upload_vertices(data.vertices.data(),
                          static_cast<uint32_t>(data.vertices.size() *
                                                sizeof(gryce_engine::assets::MeshVertex)),
                          static_cast<uint32_t>(data.vertices.size()));
    if (!data.indices.empty()) {
        mesh->upload_indices(data.indices.data(),
                             static_cast<uint32_t>(data.indices.size() * sizeof(uint32_t)),
                             static_cast<uint32_t>(data.indices.size()));
    }
    set_mesh_layout(mesh);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Renderer 实现
// ---------------------------------------------------------------------------
struct Renderer::Impl {
    RendererConfig config;
    std::unique_ptr<gryce_engine::platform::Window> window;
    std::unique_ptr<gryce_engine::render::RenderContext> ctx;
    std::unique_ptr<gryce_engine::render::RenderPipeline> pipeline;
    std::unique_ptr<gryce_engine::render::IRenderer2D> renderer2d;
    gryce_engine::platform::InputManager input;

    // 资源所有权（Renderer 创建的资源由 Renderer 统一释放）
    std::unordered_map<const IMesh*, gryce_engine::render::RHIMeshHandle> mesh_handles;
    std::unordered_map<const ITexture*, gryce_engine::render::RHITextureHandle> texture_handles;
    std::vector<std::unique_ptr<gryce_engine::render::Material>> materials;
    std::vector<gryce_engine::render::RHIMeshHandle> meshes;
    std::vector<gryce_engine::render::RHITextureHandle> textures;
    std::vector<gryce_engine::render::RHIShaderHandle> shaders;

    ui::UIManager* ui_manager = nullptr;
};

Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() { delete impl_; }

Renderer* Renderer::create(const RendererConfig& config) {
    gryce_engine::utils::glog_initialize();
    auto* r = new Renderer();
    Impl& d = *r->impl_;
    d.config = config;

    gryce_engine::resources::Project::instance().set_root(find_project_root());

    if (!gryce_engine::platform::Window::init_sdk()) {
        GLOG_ERROR("Renderer::create: GLFW 初始化失败");
        delete r;
        return nullptr;
    }

    const bool vulkan = (config.api == RenderAPI::Vulkan);
    const auto ctx_type = vulkan
        ? gryce_engine::platform::WindowContextType::NoApi
        : gryce_engine::platform::WindowContextType::OpenGL;
    const std::string title = config.title ? config.title : "Gryce";

    d.window = std::make_unique<gryce_engine::platform::Window>(
        title, config.width, config.height,
        gryce_engine::platform::WindowMode::Windowed, ctx_type);
    if (!d.window->is_valid()) {
        GLOG_ERROR("Renderer::create: 窗口创建失败");
        d.window.reset();
        gryce_engine::platform::Window::shutdown_sdk();
        delete r;
        return nullptr;
    }
    d.window->set_vsync(config.vsync);

    d.ctx = std::make_unique<gryce_engine::render::RenderContext>();
    auto backend = gryce_engine::render::create_render_backend(config.api);
    if (!backend || !d.ctx->init(d.window->native_handle(), std::move(backend))) {
        GLOG_ERROR("Renderer::create: 渲染上下文初始化失败");
        d.ctx.reset();
        d.window.reset();
        gryce_engine::platform::Window::shutdown_sdk();
        delete r;
        return nullptr;
    }

    d.window->set_resize_callback([&d](int w, int h) {
        if (d.ctx) d.ctx->set_viewport(0, 0, w, h);
        if (d.pipeline) {
            // 管线视口必须与窗口同步，否则 tonemap/后处理只覆盖旧尺寸区域，
            // 窗口其余部分残留上一帧内容 → 背景闪烁/拖影。
            d.pipeline->set_viewport(w, h);
        }
    });

    d.pipeline = std::make_unique<gryce_engine::render::RenderPipeline>();
    d.pipeline->set_shadow_map_size(config.shadow_map_size);
    d.pipeline->set_hdr_enabled(config.hdr);
    // 必须在 ctx->start() 之前初始化（主线程持有 GPU context）
    if (!d.pipeline->init(d.ctx.get(), "res:/shaders")) {
        GLOG_ERROR("Renderer::create: 渲染管线初始化失败");
        d.pipeline.reset();
        d.ctx.reset();
        d.window.reset();
        gryce_engine::platform::Window::shutdown_sdk();
        delete r;
        return nullptr;
    }

    // 2D 渲染器（UI 模块使用）：同样必须在 start() 之前 init
    auto renderer2d = d.ctx->create_renderer2d();
    if (renderer2d) {
        renderer2d->init(d.ctx.get());
        d.renderer2d = std::move(renderer2d);
    }

#if defined(GRYCE_ENABLE_UI)
    g_char_callback_renderer = r;
    glfwSetCharCallback(d.window->native_handle(), &on_char_callback);
#endif

    d.ctx->start();
    return r;
}

void Renderer::destroy() {
    if (!impl_) return;
    Impl& d = *impl_;

    // 退出顺序与 3dtest 保持一致：
    // pause 渲染线程 → 释放管线/上下文 → 销毁窗口 → GLFW shutdown
    if (d.ctx) {
        d.ctx->pause_render_thread_keep_cmdbuffer();
    }
    if (d.pipeline) {
        d.pipeline->shutdown();
        d.pipeline.reset();
    }
    if (d.renderer2d) {
        d.renderer2d->shutdown();
        d.renderer2d.reset();
    }
    if (d.ctx) {
        d.ctx->shutdown();
        d.ctx.reset();
    }
    if (d.window) {
        d.window->destroy();
        d.window.reset();
    }
    gryce_engine::platform::Window::shutdown_sdk();

#if defined(GRYCE_ENABLE_UI)
    g_char_callback_renderer = nullptr;
#endif
    delete this;
}

void Renderer::begin_frame() {
    if (!impl_) return;
    Impl& d = *impl_;
    d.window->poll_events();
    d.window->update_frame_stats();
    d.input.update(d.window.get());

#if defined(GRYCE_ENABLE_UI)
    // 输入转发到 UI（鼠标位置/按键/按钮）
    if (d.ui_manager) {
        d.ui_manager->on_mouse_move(static_cast<float>(d.input.mouse_x()),
                                    static_cast<float>(d.input.mouse_y()));
        for (int b = 0; b < 3; ++b) {
            if (d.input.is_mouse_button_pressed(b)) d.ui_manager->on_mouse_button(b, true);
            if (d.input.is_mouse_button_released(b)) d.ui_manager->on_mouse_button(b, false);
        }
        for (int k = 0; k < 512; ++k) {
            if (d.input.is_key_pressed(k)) d.ui_manager->on_keyboard(k, true);
            if (d.input.is_key_released(k)) d.ui_manager->on_keyboard(k, false);
        }
    }
#endif
}

void Renderer::end_frame() {
    if (!impl_) return;
    Impl& d = *impl_;
    // 清屏：每帧必须清除颜色+深度缓冲，否则上一帧的深度值会错误地拒绝
    // 本帧的天空盒/3D物体（深度测试通过但值更远），导致旧 UI 残留闪烁。
    // 无论是否有 3D 提交都需要清除——UI-only 帧靠此清除，3D 帧的 forward pass
    // 并未清除，依赖此处先清。
    if (d.ctx) {
        d.ctx->clear(0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (d.pipeline && d.pipeline->has_submitted_items()) {
        d.pipeline->render_submitted(*d.ctx);
    }
#if defined(GRYCE_ENABLE_UI)
    // UI 层（3D 之后绘制，覆盖在画面上）
    if (d.ui_manager) {
        d.ui_manager->render();
    }
#endif
    d.ctx->present();
}

bool Renderer::is_running() {
    if (!impl_ || !impl_->window) return false;
    return !impl_->window->should_close() && !impl_->window->close_requested();
}

double Renderer::delta_time() const {
    return impl_ ? impl_->window->delta_time() : 0.0;
}

void Renderer::draw(IMesh* mesh, IMaterial* material, const math::Matrix4f& transform) {
    if (!impl_ || !mesh) return;
    Impl& d = *impl_;
    // 网格既可以是 Renderer::load_mesh/create_mesh 创建（自动注册），
    // 也可以是 ECS World 上传后经 register_mesh_mapping 注册的组件网格；
    // 统一交给 RenderPipeline 的 submit 做句柄解析。
    d.pipeline->submit(mesh, material, transform);
}

void Renderer::draw_instanced(IMesh* mesh, IMaterial* material,
                              const math::Matrix4f* transforms, int count) {
    if (!impl_ || !mesh || !transforms || count <= 0) return;
    Impl& d = *impl_;
    d.pipeline->submit_instanced(mesh, material, transforms, count);
}

void Renderer::set_camera(const math::Vector3f& position, const math::Matrix4f& view_proj) {
    if (impl_ && impl_->pipeline) {
        impl_->pipeline->set_submit_camera(position, view_proj);
    }
}

void Renderer::set_lights(const LightData* lights, int count) {
    if (impl_ && impl_->pipeline) {
        impl_->pipeline->set_submit_lights(lights, count);
    }
}

void Renderer::set_ambient(const math::Vector3f& color) {
    if (impl_ && impl_->pipeline) {
        impl_->pipeline->set_submit_ambient(color);
    }
}

IMesh* Renderer::load_mesh(const char* path) {
    if (!impl_ || !path) return nullptr;
    Impl& d = *impl_;
    auto data = gryce_engine::assets::AssetManager::instance().load_mesh(path);
    if (!data || data->vertices.empty()) {
        GLOG_ERROR("Renderer::load_mesh: 无法加载 '{}'", path);
        return nullptr;
    }

    // 创建 + 上传必须在渲染线程执行（ctx->start() 之后主线程不持有 GL context）
    gryce_engine::render::RHIMeshHandle handle;
    d.ctx->push_command([&handle, data, ctx = d.ctx.get()](gryce_engine::render::IRenderBackend*) {
        handle = ctx->create_mesh();
        auto* mesh = ctx->mesh(handle);
        if (mesh) upload_mesh_data(mesh, *data);
    });
    d.ctx->present(); // 阻塞等待渲染线程完成上传

    if (!handle.is_valid()) return nullptr;
    IMesh* mesh = d.ctx->mesh(handle);
    if (!mesh) {
        d.ctx->destroy_mesh(handle);
        return nullptr;
    }
    d.meshes.push_back(handle);
    d.mesh_handles[mesh] = handle;
    d.pipeline->register_mesh_mapping(mesh, handle);
    return mesh;
}

IMesh* Renderer::create_mesh(const float* vertices, int vcount,
                             const uint32_t* indices, int icount) {
    if (!impl_ || !vertices || vcount <= 0) return nullptr;
    Impl& d = *impl_;

    // 输入约定：每顶点 8 个 float（position(3) + normal(3) + uv(2)），
    // 布局与引擎 PBR 管线一致（stride 32，location 0/1/2）。
    constexpr int kFloatsPerVertex = 8;
    auto data = std::make_shared<gryce_engine::assets::MeshData>();
    data->vertices.reserve(static_cast<size_t>(vcount));
    for (int i = 0; i < vcount; ++i) {
        const float* v = vertices + i * kFloatsPerVertex;
        gryce_engine::assets::MeshVertex mv;
        mv.position = math::Vector3f(v[0], v[1], v[2]);
        mv.normal = math::Vector3f(v[3], v[4], v[5]);
        mv.uv = math::Vector2f(v[6], v[7]);
        data->vertices.push_back(mv);
    }
    if (indices && icount > 0) {
        data->indices.assign(indices, indices + icount);
    }

    gryce_engine::render::RHIMeshHandle handle;
    d.ctx->push_command([&handle, data, ctx = d.ctx.get()](gryce_engine::render::IRenderBackend*) {
        handle = ctx->create_mesh();
        auto* mesh = ctx->mesh(handle);
        if (mesh) upload_mesh_data(mesh, *data);
    });
    d.ctx->present();

    if (!handle.is_valid()) return nullptr;
    IMesh* mesh = d.ctx->mesh(handle);
    if (!mesh) {
        d.ctx->destroy_mesh(handle);
        return nullptr;
    }
    d.meshes.push_back(handle);
    d.mesh_handles[mesh] = handle;
    d.pipeline->register_mesh_mapping(mesh, handle);
    return mesh;
}

IMaterial* Renderer::create_material() {
    if (!impl_) return nullptr;
    auto mat = std::make_unique<gryce_engine::render::Material>();
    IMaterial* ptr = mat.get();
    impl_->materials.push_back(std::move(mat));
    return ptr;
}

IMaterial* Renderer::load_material(const char* path) {
    if (!impl_ || !path) return nullptr;
    auto mat = std::make_unique<gryce_engine::render::Material>();
    if (!mat->load_from_file(path)) {
        GLOG_ERROR("Renderer::load_material: 无法加载 '{}'", path);
        return nullptr;
    }
    IMaterial* ptr = mat.get();
    // 材质贴图上传到渲染线程（无贴图的纯色材质不需要 GPU 资源）
    if (mat->has_gpu_textures() || !mat->albedo_map_path.empty()) {
        Impl& d = *impl_;
        d.ctx->push_command([ptr, ctx = d.ctx.get()](gryce_engine::render::IRenderBackend*) {
            ptr->upload_to_gpu(ctx);
        });
        d.ctx->present();
    }
    impl_->materials.push_back(std::move(mat));
    return ptr;
}

ITexture* Renderer::load_texture(const char* path) {
    if (!impl_ || !path) return nullptr;
    Impl& d = *impl_;
    std::string resolved = gryce_engine::resources::ResourcePath::resolve(path);
    gryce_engine::render::RHITextureHandle handle;
    d.ctx->push_command([&handle, resolved, ctx = d.ctx.get()](gryce_engine::render::IRenderBackend*) {
        handle = ctx->create_texture();
        auto* tex = ctx->texture(handle);
        if (tex) tex->load_from_file(resolved);
    });
    d.ctx->present();
    if (!handle.is_valid()) return nullptr;
    ITexture* tex = d.ctx->texture(handle);
    if (!tex || !tex->is_valid()) {
        if (tex) tex->unbind();
        d.ctx->destroy_texture(handle);
        return nullptr;
    }
    d.textures.push_back(handle);
    d.texture_handles[tex] = handle;
    return tex;
}

IShader* Renderer::load_shader(const char* vert_path, const char* frag_path) {
    if (!impl_ || !vert_path || !frag_path) return nullptr;
    Impl& d = *impl_;

    auto read_file = [](const std::string& p) -> std::string {
        std::ifstream f(gryce_engine::resources::ResourcePath::resolve(p), std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string vs = read_file(vert_path);
    const std::string fs = read_file(frag_path);
    if (vs.empty() || fs.empty()) {
        GLOG_ERROR("Renderer::load_shader: 无法读取 '{}' / '{}'", vert_path, frag_path);
        return nullptr;
    }

    gryce_engine::render::RHIShaderHandle handle;
    d.ctx->push_command([&handle, vs, fs, ctx = d.ctx.get()](gryce_engine::render::IRenderBackend*) {
        handle = ctx->create_shader();
        auto* shader = ctx->shader(handle);
        if (shader) shader->compile(vs, fs);
    });
    d.ctx->present();
    if (!handle.is_valid()) return nullptr;
    IShader* shader = d.ctx->shader(handle);
    if (!shader || !shader->is_valid()) {
        if (shader) shader->unbind();
        d.ctx->destroy_shader(handle);
        return nullptr;
    }
    d.shaders.push_back(handle);
    return shader;
}

void Renderer::set_shadow_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_shadow_enabled(on);
}
void Renderer::set_ssr_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_ssr_enabled(on);
}
void Renderer::set_dof_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_dof_enabled(on);
}
void Renderer::set_motion_blur_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_motion_blur_enabled(on);
}
void Renderer::set_fog_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_fog_enabled(on);
}
void Renderer::set_water_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_water_enabled(on);
}
void Renderer::set_gi_enabled(bool on) {
    if (impl_ && impl_->pipeline) impl_->pipeline->set_gi_enabled(on);
}

gryce_engine::render::RenderPipeline* Renderer::pipeline() {
    return impl_ ? impl_->pipeline.get() : nullptr;
}
gryce_engine::render::RenderContext* Renderer::context() {
    return impl_ ? impl_->ctx.get() : nullptr;
}
gryce_engine::render::IRenderer2D* Renderer::renderer2d() {
    return impl_ ? impl_->renderer2d.get() : nullptr;
}
gryce_engine::render::RHITextureHandle Renderer::texture_handle(const ITexture* tex) const {
    if (!impl_ || !tex) return {};
    auto it = impl_->texture_handles.find(tex);
    return it != impl_->texture_handles.end() ? it->second : gryce_engine::render::RHITextureHandle{};
}
void Renderer::window_size(int& w, int& h) const {
    w = 0;
    h = 0;
    if (impl_ && impl_->window) {
        impl_->window->get_size(w, h);
    }
}

bool Renderer::key_held(int key) const {
    return impl_ ? impl_->input.is_key_held(key) : false;
}

bool Renderer::key_pressed(int key) const {
    return impl_ ? impl_->input.is_key_pressed(key) : false;
}
void* Renderer::native_window() {
    return impl_ && impl_->window ? impl_->window->native_handle() : nullptr;
}

void Renderer::set_ui_manager(ui::UIManager* ui) {
#if defined(GRYCE_ENABLE_UI)
    if (impl_) impl_->ui_manager = ui;
#else
    (void)ui;
#endif
}

void Renderer::context_ui_text_input(const char* utf8) {
#if defined(GRYCE_ENABLE_UI)
    if (impl_ && impl_->ui_manager) {
        impl_->ui_manager->on_text_input(utf8);
    }
#else
    (void)utf8;
#endif
}

} // namespace GryceEngineUtils
