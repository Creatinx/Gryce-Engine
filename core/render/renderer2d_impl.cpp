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
#include "render/opengl/gl_backend.h"
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

// 光照 shader：全屏四边形，采样 albedo + normal，计算环境光 + 点光源
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
uniform vec3 uAmbientLight;
uniform vec2 uLightPos;
uniform float uLightRadius;
uniform vec3 uLightColor;
uniform float uLightIntensity;
uniform int uPassType; // 0=ambient, 1=point light
void main() {
    vec4 albedo_alpha = texture(uAlbedo, vTexCoord);
    if (albedo_alpha.a < 0.01) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 albedo = albedo_alpha.rgb;
    vec3 normal = texture(uNormal, vTexCoord).rgb * 2.0 - 1.0;
    normal = normalize(normal);

    vec3 lit = albedo * uAmbientLight;

    if (uPassType == 1 && uLightRadius > 0.0) {
        // OpenGL 纹理/屏幕坐标：vTexCoord.y=0 在底部；而 world_to_screen 返回 y=0 在顶部。
        // 将 fragment 位置转换到与光源一致的“y=0 在顶部”屏幕坐标系。
        vec2 fragPos = vec2(vTexCoord.x * uScreenSize.x, (1.0 - vTexCoord.y) * uScreenSize.y);
        vec2 toLight = uLightPos - fragPos;
        float dist = length(toLight);
        if (dist <= uLightRadius) {
            float attenuation = 1.0 - dist / uLightRadius;
            attenuation *= attenuation;
            vec3 lightDir = normalize(vec3(toLight, 0.15));
            float diff = max(dot(normal, lightDir), 0.0);
            lit += albedo * uLightColor * diff * attenuation * uLightIntensity;
        }
    }

    FragColor = vec4(lit, albedo_alpha.a);
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
    lit_batches_.reserve(16);
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
        if (gl_albedo_fbo_ != 0) {
            glDeleteFramebuffers(1, &gl_albedo_fbo_);
            gl_albedo_fbo_ = 0;
        }
        if (gl_normal_fbo_ != 0) {
            glDeleteFramebuffers(1, &gl_normal_fbo_);
            gl_normal_fbo_ = 0;
        }
        if (gl_albedo_tex_ != 0) {
            glDeleteTextures(1, &gl_albedo_tex_);
            gl_albedo_tex_ = 0;
        }
        if (gl_normal_tex_ != 0) {
            glDeleteTextures(1, &gl_normal_tex_);
            gl_normal_tex_ = 0;
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
    if (gl_albedo_tex_ == 0) glGenTextures(1, &gl_albedo_tex_);
    if (gl_normal_tex_ == 0) glGenTextures(1, &gl_normal_tex_);
    if (gl_albedo_fbo_ == 0) glGenFramebuffers(1, &gl_albedo_fbo_);
    if (gl_normal_fbo_ == 0) glGenFramebuffers(1, &gl_normal_fbo_);
}

void Renderer2D::resize_lighting_targets(int w, int h) {
    if (!context_alive() || w <= 0 || h <= 0) return;
    if (w == lighting_width_ && h == lighting_height_) return;

    ensure_lighting_resources();
    lighting_width_ = w;
    lighting_height_ = h;

    auto setup_tex = [](unsigned int tex, int tw, int th) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    setup_tex(gl_albedo_tex_, w, h);
    setup_tex(gl_normal_tex_, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, gl_albedo_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_albedo_tex_, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, gl_normal_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_normal_tex_, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    lit_batches_.clear();
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
    GLOG_DEBUG("Renderer2D: {} lit batches, gbuffer_valid={} light_valid={}",
               lit_batches_.size(), gbuffer_shader_.is_valid(), light_shader_.is_valid());
    if (!lit_batches_.empty() && context_alive() && gbuffer_shader_.is_valid() && light_shader_.is_valid() && fullscreen_mesh_.is_valid()) {
        render_lit_geometry_to_gbuffer();
    }
}

void Renderer2D::render_lit_geometry_to_gbuffer() {
    if (!ctx_ || lit_batches_.empty()) return;

    // 把批次数据拷贝到共享结构，供渲染线程使用
    auto batches_copy = std::make_shared<std::vector<LitBatch>>();
    batches_copy->reserve(lit_batches_.size());
    for (auto& batch : lit_batches_) {
        batches_copy->push_back({batch.albedo, batch.normal,
                                 std::vector<LitVertex2D>(batch.verts)});
    }
    lit_batches_.clear();

    math::Matrix4f view_proj = view_proj_;
    int target_w = static_cast<int>(screen_width_);
    int target_h = static_cast<int>(screen_height_);
    Color ambient = ambient_light_;
    auto lights_copy = std::make_shared<std::vector<PointLight>>(point_lights_);

    ctx_->push_command([this, batches_copy, view_proj, target_w, target_h, ambient, lights_copy](IRenderBackend* backend) {
        if (batches_copy->empty() || !backend) return;

        // 延迟光照目前只在 OpenGL 后端实现；Vulkan 走简化 forward lighting
        auto* gl_backend = dynamic_cast<GLBackend*>(backend);
        if (!gl_backend) {
            GLOG_WARN("Renderer2D: deferred lighting is only implemented for OpenGL backend");
            return;
        }

        // OpenGL 光照资源必须在渲染线程创建（GL context 在渲染线程）
        if (target_w > 0 && target_h > 0 &&
            (!lighting_resources_ready_ || target_w != lighting_width_ || target_h != lighting_height_)) {
            ensure_lighting_resources();
            resize_lighting_targets(target_w, target_h);
            lighting_resources_ready_ = gl_albedo_fbo_ != 0 && gl_normal_fbo_ != 0 &&
                                        gl_albedo_tex_ != 0 && gl_normal_tex_ != 0;
        }

        if (!lighting_resources_ready_) {
            GLOG_ERROR("Renderer2D: GL lighting resources not created");
            return;
        }

        IMesh* lit_mesh_ptr = ctx_->mesh(lit_mesh_);
        IShader* gbuffer_ptr = ctx_->shader(gbuffer_shader_);
        IShader* light_ptr = ctx_->shader(light_shader_);
        IMesh* fullscreen_ptr = ctx_->mesh(fullscreen_mesh_);
        if (!lit_mesh_ptr || !gbuffer_ptr || !light_ptr || !fullscreen_ptr) return;

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
        glBindFramebuffer(GL_FRAMEBUFFER, gl_albedo_fbo_);
        gl_backend->set_viewport(0, 0, lighting_width_, lighting_height_);
        gl_backend->set_blend(true);
        gl_backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
        gl_backend->clear(0.0f, 0.0f, 0.0f, 0.0f);

        gbuffer_ptr->bind();
        gbuffer_ptr->set_mat4("uViewProj", view_proj);

        for (const auto& batch : *batches_copy) {
            if (batch.verts.empty()) continue;
            bool has_albedo = (batch.albedo != nullptr);
            bool has_normal = (batch.normal != nullptr);

            lit_mesh_ptr->upload_vertices(batch.verts.data(),
                                          static_cast<uint32_t>(batch.verts.size() * sizeof(LitVertex2D)),
                                          static_cast<uint32_t>(batch.verts.size()));

            gbuffer_ptr->set_int("uHasAlbedo", has_albedo ? 1 : 0);
            gbuffer_ptr->set_int("uHasNormal", has_normal ? 1 : 0);
            if (has_albedo) batch.albedo->bind(0);
            gbuffer_ptr->set_int("uAlbedo", 0);
            if (has_normal) batch.normal->bind(1);
            gbuffer_ptr->set_int("uNormal", 1);
            lit_mesh_ptr->draw();
        }
        gbuffer_ptr->unbind();

        // 渲染法线到 normal_fb
        glBindFramebuffer(GL_FRAMEBUFFER, gl_normal_fbo_);
        gl_backend->clear(0.5f, 0.5f, 1.0f, 1.0f);

        gbuffer_ptr->bind();
        gbuffer_ptr->set_mat4("uViewProj", view_proj);

        for (const auto& batch : *batches_copy) {
            if (batch.verts.empty()) continue;
            bool has_albedo = (batch.albedo != nullptr);
            bool has_normal = (batch.normal != nullptr);

            lit_mesh_ptr->upload_vertices(batch.verts.data(),
                                          static_cast<uint32_t>(batch.verts.size() * sizeof(LitVertex2D)),
                                          static_cast<uint32_t>(batch.verts.size()));

            gbuffer_ptr->set_int("uHasAlbedo", has_albedo ? 1 : 0);
            gbuffer_ptr->set_int("uHasNormal", has_normal ? 1 : 0);
            if (has_albedo) batch.albedo->bind(0);
            gbuffer_ptr->set_int("uAlbedo", 0);
            if (has_normal) batch.normal->bind(1);
            gbuffer_ptr->set_int("uNormal", 1);
            lit_mesh_ptr->draw();
        }
        gbuffer_ptr->unbind();

        // 回到默认 framebuffer 进行光照 pass
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 光照 pass：全屏四边形混合到屏幕
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

        gl_backend->set_viewport(0, 0, lighting_width_, lighting_height_);
        gl_backend->set_blend(true);
        // 环境光底使用 SrcAlpha/OneMinusSrcAlpha 与背后内容混合
        gl_backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);

        math::Vector2f screen_size(screen_width_, screen_height_);

        light_ptr->bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_albedo_tex_);
        light_ptr->set_int("uAlbedo", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl_normal_tex_);
        light_ptr->set_int("uNormal", 1);
        light_ptr->set_vec2("uScreenSize", screen_size);
        light_ptr->set_vec3("uAmbientLight", math::Vector3f(ambient.r, ambient.g, ambient.b));

        // 环境光底
        light_ptr->set_int("uPassType", 0);
        light_ptr->set_float("uLightRadius", 0.0f);
        fullscreen_ptr->draw();

        // 点光源：加法混合，只叠加光源贡献
        gl_backend->set_blend_func(BlendFactor::One, BlendFactor::One);
        light_ptr->set_int("uPassType", 1);
        for (const auto& light : *lights_copy) {
            if (light.radius <= 0.0f) continue;
            light_ptr->set_vec2("uLightPos", light.pos);
            light_ptr->set_float("uLightRadius", light.radius);
            light_ptr->set_vec3("uLightColor", math::Vector3f(light.color.r, light.color.g, light.color.b));
            light_ptr->set_float("uLightIntensity", light.intensity);
            fullscreen_ptr->draw();
        }
        light_ptr->unbind();

        // 恢复默认混合与 viewport
        gl_backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
        gl_backend->set_viewport(0, 0, static_cast<int>(screen_width_), static_cast<int>(screen_height_));
    });
}

void Renderer2D::push_vertex(float x, float y, const Color& color, float u, float v) {
    vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void Renderer2D::push_text_vertex(float x, float y, const Color& color, float u, float v) {
    text_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

std::vector<Renderer2D::LitBatch>::iterator Renderer2D::find_lit_batch(ITexture* albedo, ITexture* normal) {
    for (auto it = lit_batches_.begin(); it != lit_batches_.end(); ++it) {
        if (it->albedo == albedo && it->normal == normal) {
            return it;
        }
    }
    lit_batches_.push_back({albedo, normal, {}});
    return std::prev(lit_batches_.end());
}

void Renderer2D::push_lit_vertex(ITexture* albedo, ITexture* normal,
                                 float x, float y, const Color& color,
                                 float u, float v, float nu, float nv) {
    auto it = find_lit_batch(albedo, normal);
    it->verts.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
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
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, albedo, normal_map, tint);
}

void Renderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                         float u0, float v0, float u1, float v1,
                                         ITexture* albedo, ITexture* normal_map,
                                         const Color& tint) {
    if (!context_alive()) return;
    GLOG_DEBUG("draw_lit_sprite_region: x={} y={} w={} h={} albedo={} normal={} tint=({},{},{})",
               x, y, w, h, (void*)albedo, (void*)normal_map, tint.r, tint.g, tint.b);

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    // normal 坐标：无 normal_map 时使用默认平面法线 (0.5,0.5)
    float nu0 = 0.0f, nv0 = 0.0f;
    float nu1 = 1.0f, nv1 = 1.0f;

    push_lit_vertex(albedo, normal_map,
                    x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map,
                    x1, y0, tint, u1, v0, nu1, nv0);
    push_lit_vertex(albedo, normal_map,
                    x1, y1, tint, u1, v1, nu1, nv1);

    push_lit_vertex(albedo, normal_map,
                    x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map,
                    x1, y1, tint, u1, v1, nu1, nv1);
    push_lit_vertex(albedo, normal_map,
                    x0, y1, tint, u0, v1, nu0, nv1);
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
