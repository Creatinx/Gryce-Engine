#include "renderer2d_impl.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include <GL/glew.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "render/render_context.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/opengl/gl_texture.h"
#include "render/opengl/gl_framebuffer.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "assets/texture_data.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

// 获取系统字体目录，避免硬编码 C:\Windows\Fonts。
static std::string get_system_font_dir() {
#ifdef _WIN32
    char win_dir[MAX_PATH] = {};
    UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir = win_dir;
        if (!dir.empty() && dir.back() != '\\') {
            dir += '\\';
        }
        return dir + "Fonts\\";
    }
    GLOG_WARN("get_system_font_dir: GetWindowsDirectoryA failed, falling back to hardcoded path");
#endif
    return "C:\\Windows\\Fonts\\";
}

} // namespace

bool Renderer2D::context_alive() const {
    return ctx_ && ctx_lifetime_ && ctx_lifetime_->alive.load();
}

// 2D Shader 源码
static const char* k_2d_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
out vec4 vColor;
out vec2 vTexCoord;
uniform mat4 uViewProj;
void main() {
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
}
)";

static const char* k_2d_frag = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform int uUseTexture; // 0=color, 1=font, 2=sprite
void main() {
    if (uUseTexture == 1) {
        float alpha = texture(uTexture, vTexCoord).r;
        float a = smoothstep(0.45, 0.55, alpha);
        if (a < 0.01) discard;
        FragColor = vec4(vColor.rgb, vColor.a * a);
    } else if (uUseTexture == 2) {
        FragColor = texture(uTexture, vTexCoord) * vColor;
    } else {
        FragColor = vColor;
    }
}
)";

// G-buffer shader：输出 albedo 到 attachment 0，法线到 attachment 1
static const char* k_gbuffer_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aNormalCoord;
out vec4 vColor;
out vec2 vTexCoord;
out vec2 vNormalCoord;
uniform mat4 uViewProj;
void main() {
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
    vNormalCoord = aNormalCoord;
}
)";

static const char* k_gbuffer_frag = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in vec2 vNormalCoord;
layout(location = 0) out vec4 Albedo;
layout(location = 1) out vec4 Normal;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform int uHasAlbedo;
uniform int uHasNormal;
void main() {
    vec4 albedo = (uHasAlbedo != 0) ? texture(uAlbedo, vTexCoord) : vec4(1.0);
    Albedo = albedo * vColor;
    if (uHasNormal != 0) {
        Normal = texture(uNormal, vNormalCoord);
    } else {
        Normal = vec4(0.5, 0.5, 1.0, 1.0); // 默认平面法线
    }
}
)";

// 光照 shader：全屏四边形，采样 albedo + normal，计算点光源
static const char* k_light_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* k_light_frag = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform vec2 uScreenSize;
uniform vec2 uLightPos;
uniform float uLightRadius;
uniform vec3 uLightColor;
uniform float uLightIntensity;
void main() {
    vec4 albedo_alpha = texture(uAlbedo, vTexCoord);
    vec3 albedo = albedo_alpha.rgb;
    vec3 normal = texture(uNormal, vTexCoord).rgb * 2.0 - 1.0;
    if (length(normal) < 0.01 || albedo_alpha.a < 0.01) {
        FragColor = vec4(0.0);
        return;
    }
    normal = normalize(normal);

    vec2 fragPos = vTexCoord * uScreenSize;
    vec2 toLight = uLightPos - fragPos;
    float dist = length(toLight);
    if (dist > uLightRadius) {
        FragColor = vec4(0.0);
        return;
    }
    float attenuation = 1.0 - dist / uLightRadius;
    attenuation *= attenuation;

    vec3 lightDir = normalize(vec3(toLight, 0.15));
    float diff = max(dot(normal, lightDir), 0.0);

    vec3 lit = albedo * uLightColor * diff * attenuation * uLightIntensity;
    FragColor = vec4(lit, 1.0);
}
)";

Renderer2D::Renderer2D() = default;

Renderer2D::~Renderer2D() {
    shutdown();
}

void Renderer2D::init(RenderContext* ctx) {
    if (initialized_ || !ctx) return;
    ctx_ = ctx;
    ctx_lifetime_ = ctx->lifetime();

    shader_ = ctx_->create_shader();
    IShader* shader_ptr = ctx_->shader(shader_);
    if (!shader_.is_valid() || !shader_ptr || !shader_ptr->compile(k_2d_vert, k_2d_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile 2D shader");
        if (shader_.is_valid()) {
            ctx_->destroy_shader(shader_);
            shader_ = RHIShaderHandle{};
        }
        return;
    }

    gbuffer_shader_ = ctx_->create_shader();
    IShader* gbuffer_ptr = ctx_->shader(gbuffer_shader_);
    if (!gbuffer_shader_.is_valid() || !gbuffer_ptr || !gbuffer_ptr->compile(k_gbuffer_vert, k_gbuffer_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile gbuffer shader");
        if (gbuffer_shader_.is_valid()) {
            ctx_->destroy_shader(gbuffer_shader_);
            gbuffer_shader_ = RHIShaderHandle{};
        }
    }

    light_shader_ = ctx_->create_shader();
    IShader* light_ptr = ctx_->shader(light_shader_);
    if (!light_shader_.is_valid() || !light_ptr || !light_ptr->compile(k_light_vert, k_light_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile light shader");
        if (light_shader_.is_valid()) {
            ctx_->destroy_shader(light_shader_);
            light_shader_ = RHIShaderHandle{};
        }
    }

    mesh_ = ctx_->create_mesh();
    lit_mesh_ = ctx_->create_mesh();
    fullscreen_mesh_ = ctx_->create_mesh();
    if (!mesh_.is_valid() || !lit_mesh_.is_valid() || !fullscreen_mesh_.is_valid()) {
        GLOG_ERROR("Renderer2D: failed to create mesh");
        shutdown();
        return;
    }

    // 优先使用项目内置 TTF（Roboto），保证跨机器一致性；失败再尝试系统字体
    {
        std::string bundled = resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf");
        if (bundled.empty() || !std::filesystem::exists(bundled)) {
            bundled = resources::Project::instance().root() + "/third_party/imgui/misc/fonts/Roboto-Medium.ttf";
        }
        if (std::filesystem::exists(bundled)) {
            GLOG_INFO("Renderer2D: trying bundled font '{}'", bundled);
            if (font_atlas_.init(ctx_, bundled, 32.0f)) {
                using_fallback_font_ = false;
                GLOG_INFO("Renderer2D: loaded bundled font '{}'", bundled);
            }
        }
    }

    // 内置字体失败时尝试系统字体
    if (!font_atlas_.texture()) {
        std::string font_dir = get_system_font_dir();
        const char* font_names[] = {
            "arial.ttf",
            "segoeui.ttf",
            "tahoma.ttf",
            "calibri.ttf",
            "verdana.ttf",
            "times.ttf",
            "msyh.ttc",
            "simhei.ttf",
            nullptr
        };

        for (int i = 0; font_names[i]; ++i) {
            std::string font_path = font_dir + font_names[i];
            if (font_atlas_.init(ctx_, font_path, 32.0f)) {
                using_fallback_font_ = false;
                break;
            }
        }
    }

    if (!font_atlas_.texture()) {
        GLOG_WARN("Renderer2D: no system font loaded, creating fallback atlas");
        if (font_atlas_.create_fallback_atlas(ctx_, 32.0f)) {
            using_fallback_font_ = true;
        } else {
            GLOG_ERROR("Renderer2D: failed to create fallback font atlas, text rendering disabled");
        }
    }

    vertices_.reserve(4096);
    text_vertices_.reserve(512);
    lit_vertices_.reserve(1024);
    initialized_ = true;
    GLOG_INFO("Renderer2D initialized");
}

void Renderer2D::shutdown() {
    if (!initialized_) return;

    if (context_alive()) {
        if (font_atlas_.texture()) {
            font_atlas_.destroy(ctx_);
        }
        if (albedo_fb_.is_valid()) {
            ctx_->destroy_framebuffer(albedo_fb_);
            albedo_fb_ = RHIFramebufferHandle{};
        }
        if (normal_fb_.is_valid()) {
            ctx_->destroy_framebuffer(normal_fb_);
            normal_fb_ = RHIFramebufferHandle{};
        }
        if (albedo_tex_.is_valid()) {
            ctx_->destroy_texture(albedo_tex_);
            albedo_tex_ = RHITextureHandle{};
        }
        if (normal_tex_.is_valid()) {
            ctx_->destroy_texture(normal_tex_);
            normal_tex_ = RHITextureHandle{};
        }
        if (mesh_.is_valid()) {
            ctx_->destroy_mesh(mesh_);
            mesh_ = RHIMeshHandle{};
        }
        if (lit_mesh_.is_valid()) {
            ctx_->destroy_mesh(lit_mesh_);
            lit_mesh_ = RHIMeshHandle{};
        }
        if (fullscreen_mesh_.is_valid()) {
            ctx_->destroy_mesh(fullscreen_mesh_);
            fullscreen_mesh_ = RHIMeshHandle{};
        }
        if (shader_.is_valid()) {
            ctx_->destroy_shader(shader_);
            shader_ = RHIShaderHandle{};
        }
        if (gbuffer_shader_.is_valid()) {
            ctx_->destroy_shader(gbuffer_shader_);
            gbuffer_shader_ = RHIShaderHandle{};
        }
        if (light_shader_.is_valid()) {
            ctx_->destroy_shader(light_shader_);
            light_shader_ = RHIShaderHandle{};
        }
    } else {
        GLOG_WARN("Renderer2D::shutdown: RenderContext already destroyed, skipping resource cleanup");
    }

    initialized_ = false;
    ctx_ = nullptr;
    ctx_lifetime_.reset();
}

void Renderer2D::ensure_lighting_resources() {
    if (!context_alive() || !ctx_) return;
    if (albedo_fb_.is_valid() && normal_fb_.is_valid() && albedo_tex_.is_valid() && normal_tex_.is_valid()) return;

    albedo_tex_ = ctx_->create_texture();
    normal_tex_ = ctx_->create_texture();
    albedo_fb_ = ctx_->create_framebuffer();
    normal_fb_ = ctx_->create_framebuffer();

    if (!albedo_tex_.is_valid() || !normal_tex_.is_valid() || !albedo_fb_.is_valid() || !normal_fb_.is_valid()) {
        GLOG_ERROR("Renderer2D: failed to create lighting resources");
    }
}

void Renderer2D::resize_lighting_targets(int w, int h) {
    if (!context_alive() || w <= 0 || h <= 0) return;
    if (w == lighting_width_ && h == lighting_height_) return;

    ensure_lighting_resources();
    if (!albedo_tex_.is_valid() || !normal_tex_.is_valid() || !albedo_fb_.is_valid() || !normal_fb_.is_valid()) return;

    lighting_width_ = w;
    lighting_height_ = h;

    ITexture* albedo_ptr = ctx_->texture(albedo_tex_);
    ITexture* normal_ptr = ctx_->texture(normal_tex_);
    IFramebuffer* albedo_fb_ptr = ctx_->framebuffer(albedo_fb_);
    IFramebuffer* normal_fb_ptr = ctx_->framebuffer(normal_fb_);
    if (!albedo_ptr || !normal_ptr || !albedo_fb_ptr || !normal_fb_ptr) return;

    albedo_ptr->create(TextureFormat::RGBA8, w, h);
    albedo_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    albedo_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    normal_ptr->create(TextureFormat::RGBA8, w, h);
    normal_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    normal_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    albedo_fb_ptr->create(w, h);
    albedo_fb_ptr->attach_color_texture(albedo_ptr);

    normal_fb_ptr->create(w, h);
    normal_fb_ptr->attach_color_texture(normal_ptr);

    if (!albedo_fb_ptr->is_complete() || !normal_fb_ptr->is_complete()) {
        GLOG_ERROR("Renderer2D: lighting framebuffer incomplete");
    }
}

void Renderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    lit_vertices_.clear();
    reset_lights();
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width, screen_height, 0.0f, -1.0f, 1.0f);

    math::Vector2f screen_center(screen_width * 0.5f, screen_height * 0.5f);
    math::Matrix4f view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                        * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                        * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    view_proj_ = ortho_ * view;

    if (context_alive()) {
        ctx_->set_depth_test(false);
        ctx_->set_blend(true);
        ctx_->set_cull_face(false);
    }
}

void Renderer2D::set_camera(const math::Vector2f& center, float zoom) {
    camera_center_ = center;
    camera_zoom_ = zoom <= 0.0f ? 1.0f : zoom;
}

math::Vector2f Renderer2D::world_to_screen(const math::Vector2f& world) const {
    math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
    return screen_center + (world - camera_center_) * camera_zoom_;
}

void Renderer2D::end_frame() {
    if (context_alive()) {
        ctx_->set_depth_test(false);
        ctx_->set_blend(true);
        ctx_->set_cull_face(false);
    }

    // 1. 先渲染不受光照的 2D 几何体到屏幕
    flush_batches();

    // 2. 若有受光照精灵，执行延迟光照
    if (!lit_vertices_.empty() && context_alive() && gbuffer_shader_.is_valid() && light_shader_.is_valid() && fullscreen_mesh_.is_valid()) {
        render_lit_geometry_to_gbuffer();
    }
}

void Renderer2D::render_lit_geometry_to_gbuffer() {
    if (!ctx_ || lit_vertices_.empty()) return;

    auto verts_shared = std::make_shared<std::vector<LitVertex2D>>(std::move(lit_vertices_));
    lit_vertices_.clear();

    math::Matrix4f view_proj = view_proj_;
    int target_w = static_cast<int>(screen_width_);
    int target_h = static_cast<int>(screen_height_);
    Color ambient = ambient_light_;
    auto lights_copy = std::make_shared<std::vector<PointLight>>(point_lights_);

    ctx_->push_command([this, verts_shared, view_proj, target_w, target_h, ambient, lights_copy](IRenderBackend* backend) {
        if (verts_shared->empty()) return;

        // 延迟创建/调整光照目标尺寸（必须在渲染线程执行 GL 操作）
        resize_lighting_targets(target_w, target_h);
        if (!albedo_fb_.is_valid() || !normal_fb_.is_valid() || !albedo_tex_.is_valid() || !normal_tex_.is_valid()) {
            GLOG_ERROR("Renderer2D: failed to create lighting resources on render thread");
            return;
        }

        IMesh* lit_mesh_ptr = ctx_->mesh(lit_mesh_);
        IShader* gbuffer_ptr = ctx_->shader(gbuffer_shader_);
        IShader* light_ptr = ctx_->shader(light_shader_);
        IMesh* fullscreen_ptr = ctx_->mesh(fullscreen_mesh_);
        ITexture* albedo_tex_ptr = ctx_->texture(albedo_tex_);
        ITexture* normal_tex_ptr = ctx_->texture(normal_tex_);
        if (!lit_mesh_ptr || !gbuffer_ptr || !light_ptr || !fullscreen_ptr || !albedo_tex_ptr || !normal_tex_ptr) return;

        lit_mesh_ptr->upload_vertices(verts_shared->data(),
                                    static_cast<uint32_t>(verts_shared->size() * sizeof(LitVertex2D)),
                                    static_cast<uint32_t>(verts_shared->size()));
        VertexLayout layout;
        layout.stride = sizeof(LitVertex2D);
        layout.attributes = {
            {0, VertexType::Float2, false, 0},
            {1, VertexType::Float4, false, 2 * sizeof(float)},
            {2, VertexType::Float2, false, 6 * sizeof(float)},
            {3, VertexType::Float2, false, 8 * sizeof(float)}
        };
        lit_mesh_ptr->set_layout(layout);

        // 渲染 albedo 到 albedo_fb
        ctx_->set_framebuffer(albedo_fb_);
        ctx_->set_viewport(0, 0, lighting_width_, lighting_height_);
        ctx_->set_blend(true);
        backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);

        gbuffer_ptr->bind();
        gbuffer_ptr->set_mat4("uViewProj", view_proj);
        gbuffer_ptr->set_int("uHasAlbedo", 0);
        gbuffer_ptr->set_int("uHasNormal", 0);
        lit_mesh_ptr->draw();
        gbuffer_ptr->unbind();

        // 渲染法线到 normal_fb（使用默认平面法线）
        ctx_->set_framebuffer(normal_fb_);
        ctx_->clear(0.5f, 0.5f, 1.0f, 1.0f);

        gbuffer_ptr->bind();
        gbuffer_ptr->set_mat4("uViewProj", view_proj);
        gbuffer_ptr->set_int("uHasAlbedo", 0);
        gbuffer_ptr->set_int("uHasNormal", 0);
        lit_mesh_ptr->draw();
        gbuffer_ptr->unbind();

        ctx_->set_framebuffer(RHIFramebufferHandle{});

        // 光照 pass：全屏四边形累加到屏幕
        struct FSVertex {
            float x, y, u, v;
        };
        FSVertex fs_quad[6] = {
            {-1.0f, -1.0f, 0.0f, 0.0f},
            { 1.0f, -1.0f, 1.0f, 0.0f},
            { 1.0f,  1.0f, 1.0f, 1.0f},
            {-1.0f, -1.0f, 0.0f, 0.0f},
            { 1.0f,  1.0f, 1.0f, 1.0f},
            {-1.0f,  1.0f, 0.0f, 1.0f}
        };

        fullscreen_ptr->upload_vertices(fs_quad, sizeof(fs_quad), 6);
        VertexLayout fs_layout;
        fs_layout.stride = sizeof(FSVertex);
        fs_layout.attributes = {
            {0, VertexType::Float2, false, 0},
            {1, VertexType::Float2, false, 2 * sizeof(float)}
        };
        fullscreen_ptr->set_layout(fs_layout);

        ctx_->set_viewport(0, 0, lighting_width_, lighting_height_);
        ctx_->set_blend(true);
        backend->set_blend_func(BlendFactor::One, BlendFactor::One); // 累加混合

        math::Vector2f screen_size(screen_width_, screen_height_);

        light_ptr->bind();
        albedo_tex_ptr->bind(0);
        light_ptr->set_int("uAlbedo", 0);
        normal_tex_ptr->bind(1);
        light_ptr->set_int("uNormal", 1);
        light_ptr->set_vec2("uScreenSize", screen_size);

        // 环境光底
        if (ambient.r > 0.0f || ambient.g > 0.0f || ambient.b > 0.0f) {
            light_ptr->set_vec2("uLightPos", math::Vector2f(-100000.0f, -100000.0f));
            light_ptr->set_float("uLightRadius", 0.0f);
            light_ptr->set_vec3("uLightColor", math::Vector3f(ambient.r, ambient.g, ambient.b));
            light_ptr->set_float("uLightIntensity", 1.0f);
            fullscreen_ptr->draw();
        }

        // 点光源累加
        for (const auto& light : *lights_copy) {
            light_ptr->set_vec2("uLightPos", light.pos);
            light_ptr->set_float("uLightRadius", light.radius);
            light_ptr->set_vec3("uLightColor", math::Vector3f(light.color.r, light.color.g, light.color.b));
            light_ptr->set_float("uLightIntensity", light.intensity);
            fullscreen_ptr->draw();
        }
        light_ptr->unbind();

        // 恢复默认混合
        backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
    });
}

void Renderer2D::push_vertex(float x, float y, const Color& color, float u, float v) {
    vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void Renderer2D::push_text_vertex(float x, float y, const Color& color, float u, float v) {
    text_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void Renderer2D::push_lit_vertex(float x, float y, const Color& color, float u, float v, float nu, float nv) {
    lit_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
}

void Renderer2D::flush_batches() {
    flush_batch(std::move(vertices_), false);
    flush_batch(std::move(text_vertices_), true);
}

void Renderer2D::flush_batch(std::vector<Vertex2D>&& verts, bool is_text) {
    if (verts.empty() || !context_alive() || !mesh_.is_valid() || !shader_.is_valid()) {
        if (is_text) {
            text_vertices_.clear();
        } else {
            vertices_.clear();
        }
        return;
    }

    math::Matrix4f view_proj = view_proj_;
    ITexture* font_tex = is_text ? font_atlas_.texture() : nullptr;

    auto verts_shared = std::make_shared<std::vector<Vertex2D>>(std::move(verts));

    ctx_->push_command([this, verts_shared, view_proj, is_text, font_tex](IRenderBackend*) {
        IMesh* mesh_ptr = ctx_->mesh(mesh_);
        IShader* shader_ptr = ctx_->shader(shader_);
        if (!mesh_ptr || !shader_ptr) return;

        mesh_ptr->upload_vertices(verts_shared->data(), static_cast<uint32_t>(verts_shared->size() * sizeof(Vertex2D)), static_cast<uint32_t>(verts_shared->size()));

        VertexLayout layout;
        layout.stride = sizeof(Vertex2D);
        layout.attributes = {
            {0, VertexType::Float2, false, 0},
            {1, VertexType::Float4, false, 2 * sizeof(float)},
            {2, VertexType::Float2, false, 6 * sizeof(float)}
        };
        mesh_ptr->set_layout(layout);

        shader_ptr->bind();
        shader_ptr->set_mat4("uViewProj", view_proj);

        if (is_text && font_tex) {
            font_tex->bind(0);
            shader_ptr->set_int("uTexture", 0);
            shader_ptr->set_int("uUseTexture", 1);
        } else {
            shader_ptr->set_int("uUseTexture", 0);
        }

        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::draw_rect(float x, float y, float w, float h, const Color& color) {
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);
    push_vertex(x0, y1, color, 0, 0);
}

void Renderer2D::draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) {
    if (points.size() < 3) return;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        push_vertex(points[0].x, points[0].y, color, 0, 0);
        push_vertex(points[i].x, points[i].y, color, 0, 0);
        push_vertex(points[i + 1].x, points[i + 1].y, color, 0, 0);
    }
}

void Renderer2D::draw_circle(float cx, float cy, float r, int segments, const Color& color) {
    if (segments < 3) segments = 3;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < segments; ++i) {
        float a0 = 2.0f * pi * static_cast<float>(i) / static_cast<float>(segments);
        float a1 = 2.0f * pi * static_cast<float>(i + 1) / static_cast<float>(segments);
        float x0 = cx + r * std::cos(a0);
        float y0 = cy + r * std::sin(a0);
        float x1 = cx + r * std::cos(a1);
        float y1 = cy + r * std::sin(a1);

        push_vertex(cx, cy, color, 0, 0);
        push_vertex(x0, y0, color, 0, 0);
        push_vertex(x1, y1, color, 0, 0);
    }
}

void Renderer2D::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {
    if (!font_atlas_.texture()) {
        float cursor_x = x;
        float cursor_y = y;
        float block_w = font_size * 0.6f;
        float block_h = font_size;
        for (char c : text) {
            if (c == '\n') {
                cursor_x = x;
                cursor_y += block_h;
                continue;
            }
            if (c != ' ') {
                draw_rect(cursor_x, cursor_y - block_h * 0.8f, block_w, block_h, color);
            }
            cursor_x += block_w;
        }
        return;
    }

    float scale = font_size / font_atlas_.font_size();
    float cursor_x = x;
    float cursor_y = y;

    for (char c : text) {
        if (c == '\n') {
            cursor_x = x;
            cursor_y += font_atlas_.font_size() * scale;
            continue;
        }

        const Glyph* g = font_atlas_.get_glyph(c);
        if (!g) continue;

        float x0 = cursor_x + g->offset_x * scale;
        float y0 = cursor_y + g->offset_y * scale;
        float x1 = x0 + g->width * scale;
        float y1 = y0 + g->height * scale;

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y0, color, g->uv1_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);
        push_text_vertex(x0, y1, color, g->uv0_x, g->uv1_y);

        cursor_x += g->advance * scale;
    }
}

void Renderer2D::set_ambient_light(const Color& color) {
    ambient_light_ = color;
}

void Renderer2D::add_point_light(const math::Vector2f& pos, float radius,
                                  const Color& color, float intensity) {
    point_lights_.push_back({pos, radius, color, intensity});
}

void Renderer2D::reset_lights() {
    point_lights_.clear();
    ambient_light_ = Color::black();
}

void Renderer2D::draw_sprite(float x, float y, float w, float h,
                              ITexture* texture, const Color& tint) {
    draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

void Renderer2D::draw_sprite_region(float x, float y, float w, float h,
                                     float u0, float v0, float u1, float v1,
                                     ITexture* texture, const Color& tint) {
    if (!texture || !context_alive() || !mesh_.is_valid() || !shader_.is_valid()) return;

    // 先提交当前普通批次，避免纹理状态混合
    flush_batch(std::move(vertices_), false);

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    ctx_->push_command([this, x0, y0, x1, y1, u0, v0, u1, v1, texture, tint](IRenderBackend* backend) {
        (void)backend;
        Vertex2D verts[6] = {
            {x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0},
            {x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0},
            {x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1},
            {x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0},
            {x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1},
            {x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1},
        };

        IMesh* mesh_ptr = ctx_->mesh(mesh_);
        IShader* shader_ptr = ctx_->shader(shader_);
        if (!mesh_ptr || !shader_ptr) return;

        mesh_ptr->upload_vertices(verts, static_cast<uint32_t>(sizeof(verts)), 6);
        VertexLayout layout;
        layout.stride = sizeof(Vertex2D);
        layout.attributes = {
            {0, VertexType::Float2, false, 0},
            {1, VertexType::Float4, false, 2 * sizeof(float)},
            {2, VertexType::Float2, false, 6 * sizeof(float)}
        };
        mesh_ptr->set_layout(layout);

        shader_ptr->bind();
        shader_ptr->set_mat4("uViewProj", view_proj_);
        texture->bind(0);
        shader_ptr->set_int("uTexture", 0);
        shader_ptr->set_int("uUseTexture", 2);
        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::draw_lit_sprite(float x, float y, float w, float h,
                                  ITexture* albedo, ITexture* normal_map,
                                  const Color& tint) {
    (void)albedo;
    (void)normal_map;

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_lit_vertex(x0, y0, tint, 0.0f, 0.0f, 0.0f, 0.0f);
    push_lit_vertex(x1, y0, tint, 1.0f, 0.0f, 1.0f, 0.0f);
    push_lit_vertex(x1, y1, tint, 1.0f, 1.0f, 1.0f, 1.0f);

    push_lit_vertex(x0, y0, tint, 0.0f, 0.0f, 0.0f, 0.0f);
    push_lit_vertex(x1, y1, tint, 1.0f, 1.0f, 1.0f, 1.0f);
    push_lit_vertex(x0, y1, tint, 0.0f, 1.0f, 0.0f, 1.0f);
}

void Renderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                         float u0, float v0, float u1, float v1,
                                         ITexture* albedo, ITexture* normal_map,
                                         const Color& tint) {
    // 当前 2D 光照批次只支持单张图集；多图集 lit sprite 先退化到非光照绘制
    (void)normal_map;
    if (albedo) {
        draw_sprite_region(x, y, w, h, u0, v0, u1, v1, albedo, tint);
    } else {
        draw_rect(x, y, w, h, tint);
    }
}

RHITextureHandle Renderer2D::create_texture_from_data(const assets::TextureData* data) {
    if (!data || data->empty() || !context_alive() || !ctx_) return RHITextureHandle{};

    RHITextureHandle tex = ctx_->create_texture();
    if (!tex.is_valid()) return RHITextureHandle{};

    // 在渲染线程上传 GPU；主线程只创建对象
    ctx_->push_command([this, tex, data](IRenderBackend*) {
        ITexture* tex_ptr = ctx_->texture(tex);
        if (!tex_ptr) return;
        tex_ptr->upload_data(data->data(), data->width, data->height, data->channels);
        tex_ptr->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    });

    return tex;
}

ITexture* Renderer2D::resolve_texture(RHITextureHandle handle) const {
    return ctx_ ? ctx_->texture(handle) : nullptr;
}

} // namespace gryce_engine::render
