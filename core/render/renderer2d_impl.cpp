#include "renderer2d_impl.h"

#include <array>
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
#include "render/opengl/gl_shader.h"
#include "render/opengl/gl_utils.h"
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

// ---------------------------------------------------------------------------
// 2D Shader 源码
// ---------------------------------------------------------------------------
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

static const char* k_lit_sprite_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aNormalCoord;
out vec4 vColor;
out vec2 vTexCoord;
out vec2 vNormalCoord;
out vec2 vWorldPos;
uniform mat4 uViewProj;
void main() {
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
    vNormalCoord = aNormalCoord;
    vWorldPos = aPos;
}
)";

static const char* k_lit_sprite_frag = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in vec2 vNormalCoord;
in vec2 vWorldPos;
out vec4 FragColor;

uniform sampler2D uAlbedo;
uniform sampler2D uNormalMap;
uniform vec3 uAmbientLight;
uniform int uLightCount;

#define MAX_LIGHTS 32
uniform int uLightType[MAX_LIGHTS];
uniform vec2 uLightPos[MAX_LIGHTS];
uniform vec2 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];
uniform float uLightRadius[MAX_LIGHTS];
uniform float uLightRange[MAX_LIGHTS];
uniform float uLightSpotAngle[MAX_LIGHTS];
uniform float uLightSpotSoftness[MAX_LIGHTS];

uniform sampler2D uShadowMap;
uniform mat4 uLightSpaceMatrix;
uniform int uUseShadowMap;
uniform int uShadowLightIndex;

float compute_shadow(vec4 light_space_pos) {
    vec3 proj = light_space_pos.xyz / light_space_pos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 0.0;
    float closest = texture(uShadowMap, proj.xy).r;
    float current = proj.z;
    return current > closest + 0.005 ? 0.0 : 1.0;
}

void main() {
    vec4 albedo = texture(uAlbedo, vTexCoord) * vColor;
    if (albedo.a < 0.01) discard;

    vec3 normal = texture(uNormalMap, vNormalCoord).rgb;
    normal = normalize(normal * 2.0 - 1.0);
    if (normal.z < 0.0) normal.z = -normal.z;

    vec3 lit = albedo.rgb * uAmbientLight;

    for (int i = 0; i < uLightCount; ++i) {
        int type = uLightType[i];
        vec3 light_color = uLightColor[i] * uLightIntensity[i];
        vec3 L;
        float attenuation = 1.0;
        float spot_factor = 1.0;

        if (type == 0) {
            vec2 to_light = uLightPos[i] - vWorldPos;
            float dist = length(to_light);
            if (dist > uLightRadius[i]) continue;
            attenuation = 1.0 - dist / uLightRadius[i];
            attenuation *= attenuation;
            L = normalize(vec3(to_light, 0.15));
        } else if (type == 1) {
            L = normalize(vec3(-uLightDir[i], 0.15));
        } else {
            vec2 to_light = uLightPos[i] - vWorldPos;
            float dist = length(to_light);
            if (dist > uLightRange[i]) continue;
            attenuation = 1.0 - dist / uLightRange[i];
            attenuation *= attenuation;
            L = normalize(vec3(to_light, 0.15));
            vec2 spot_dir = normalize(uLightDir[i]);
            float cos_angle = dot(normalize(-to_light), spot_dir);
            float outer = cos(radians(uLightSpotAngle[i]));
            float inner = cos(radians(uLightSpotAngle[i] * (1.0 - uLightSpotSoftness[i])));
            spot_factor = smoothstep(outer, inner, cos_angle);
            if (spot_factor <= 0.0) continue;
        }

        float diff = max(dot(normal, L), 0.0);

        float shadow = 1.0;
        if (uUseShadowMap == 1 && uShadowLightIndex == i) {
            vec4 light_space = uLightSpaceMatrix * vec4(vWorldPos, 0.0, 1.0);
            shadow = compute_shadow(light_space);
        }

        lit += albedo.rgb * light_color * diff * attenuation * spot_factor * shadow;
    }

    FragColor = vec4(lit, albedo.a);
}
)";

static const char* k_shadow_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uLightSpaceMatrix;
void main() {
    gl_Position = uLightSpaceMatrix * vec4(aPos, 0.0, 1.0);
}
)";

static const char* k_shadow_frag = R"(
#version 330 core
void main() {
}
)";

static const char* k_bloom_threshold_frag = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform float uThreshold;
void main() {
    vec3 color = texture(uTexture, vTexCoord).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    FragColor = brightness > uThreshold ? vec4(color, 1.0) : vec4(0.0);
}
)";

static const char* k_bloom_blur_frag = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec2 uDirection;
uniform vec2 uTexelSize;
void main() {
    vec2 off1 = vec2(1.3846153846) * uDirection * uTexelSize;
    vec2 off2 = vec2(3.2307692308) * uDirection * uTexelSize;
    FragColor = texture(uTexture, vTexCoord) * 0.2270270270;
    FragColor += texture(uTexture, vTexCoord + off1) * 0.3162162162;
    FragColor += texture(uTexture, vTexCoord - off1) * 0.3162162162;
    FragColor += texture(uTexture, vTexCoord + off2) * 0.0702702703;
    FragColor += texture(uTexture, vTexCoord - off2) * 0.0702702703;
}
)";

static const char* k_bloom_compose_frag = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uIntensity;
void main() {
    vec3 scene = texture(uScene, vTexCoord).rgb;
    vec3 bloom = texture(uBloom, vTexCoord).rgb;
    FragColor = vec4(scene + bloom * uIntensity, 1.0);
}
)";

static const char* k_fullscreen_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
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

    lit_sprite_shader_ = ctx_->create_shader();
    IShader* lit_ptr = ctx_->shader(lit_sprite_shader_);
    if (!lit_sprite_shader_.is_valid() || !lit_ptr || !lit_ptr->compile(k_lit_sprite_vert, k_lit_sprite_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile lit sprite shader");
        if (lit_sprite_shader_.is_valid()) {
            ctx_->destroy_shader(lit_sprite_shader_);
            lit_sprite_shader_ = RHIShaderHandle{};
        }
    }

    shadow_shader_ = ctx_->create_shader();
    IShader* shadow_ptr = ctx_->shader(shadow_shader_);
    if (!shadow_shader_.is_valid() || !shadow_ptr || !shadow_ptr->compile(k_shadow_vert, k_shadow_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile shadow shader");
        if (shadow_shader_.is_valid()) {
            ctx_->destroy_shader(shadow_shader_);
            shadow_shader_ = RHIShaderHandle{};
        }
    }

    bloom_threshold_shader_ = ctx_->create_shader();
    IShader* bloom_threshold_ptr = ctx_->shader(bloom_threshold_shader_);
    if (!bloom_threshold_shader_.is_valid() || !bloom_threshold_ptr ||
        !bloom_threshold_ptr->compile(k_fullscreen_vert, k_bloom_threshold_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile bloom threshold shader");
        if (bloom_threshold_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_threshold_shader_);
            bloom_threshold_shader_ = RHIShaderHandle{};
        }
    }

    bloom_blur_shader_ = ctx_->create_shader();
    IShader* bloom_blur_ptr = ctx_->shader(bloom_blur_shader_);
    if (!bloom_blur_shader_.is_valid() || !bloom_blur_ptr ||
        !bloom_blur_ptr->compile(k_fullscreen_vert, k_bloom_blur_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile bloom blur shader");
        if (bloom_blur_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_blur_shader_);
            bloom_blur_shader_ = RHIShaderHandle{};
        }
    }

    bloom_compose_shader_ = ctx_->create_shader();
    IShader* bloom_compose_ptr = ctx_->shader(bloom_compose_shader_);
    if (!bloom_compose_shader_.is_valid() || !bloom_compose_ptr ||
        !bloom_compose_ptr->compile(k_fullscreen_vert, k_bloom_compose_frag)) {
        GLOG_ERROR("Renderer2D: failed to compile bloom compose shader");
        if (bloom_compose_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_compose_shader_);
            bloom_compose_shader_ = RHIShaderHandle{};
        }
    }

    mesh_ = ctx_->create_mesh();
    if (!mesh_.is_valid()) {
        GLOG_ERROR("Renderer2D: failed to create mesh");
        shutdown();
        return;
    }

    create_shadow_map();
    create_bloom_targets();

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
    shadow_caster_vertices_.reserve(512);
    initialized_ = true;
    GLOG_INFO("Renderer2D initialized");
}

void Renderer2D::shutdown() {
    if (!initialized_) return;

    if (context_alive()) {
        if (font_atlas_.texture()) {
            font_atlas_.destroy(ctx_);
        }
        destroy_shadow_map();
        destroy_bloom_targets();
        if (mesh_.is_valid()) {
            ctx_->destroy_mesh(mesh_);
            mesh_ = RHIMeshHandle{};
        }
        if (shader_.is_valid()) {
            ctx_->destroy_shader(shader_);
            shader_ = RHIShaderHandle{};
        }
        if (lit_sprite_shader_.is_valid()) {
            ctx_->destroy_shader(lit_sprite_shader_);
            lit_sprite_shader_ = RHIShaderHandle{};
        }
        if (shadow_shader_.is_valid()) {
            ctx_->destroy_shader(shadow_shader_);
            shadow_shader_ = RHIShaderHandle{};
        }
        if (bloom_threshold_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_threshold_shader_);
            bloom_threshold_shader_ = RHIShaderHandle{};
        }
        if (bloom_blur_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_blur_shader_);
            bloom_blur_shader_ = RHIShaderHandle{};
        }
        if (bloom_compose_shader_.is_valid()) {
            ctx_->destroy_shader(bloom_compose_shader_);
            bloom_compose_shader_ = RHIShaderHandle{};
        }
    } else {
        GLOG_WARN("Renderer2D::shutdown: RenderContext already destroyed, skipping resource cleanup");
    }

    initialized_ = false;
    ctx_ = nullptr;
    ctx_lifetime_.reset();
}

bool Renderer2D::create_shadow_map() {
    if (!ctx_) return false;
    shadow_map_ = ctx_->create_texture();
    ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
    if (!shadow_map_.is_valid() || !shadow_map_ptr ||
        !shadow_map_ptr->create_depth(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("Renderer2D: failed to create shadow map texture");
        return false;
    }
    shadow_map_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    shadow_map_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    // 2D 阴影贴图使用普通 sampler2D 采样，必须关闭深度比较模式。
    auto* gl_shadow = static_cast<GLTexture*>(shadow_map_ptr);
    if (gl_shadow && gl_shadow->texture_id()) {
        if (gl_dsa_available()) {
            glTextureParameteri(gl_shadow->texture_id(), GL_TEXTURE_COMPARE_MODE, GL_NONE);
        } else {
            glBindTexture(GL_TEXTURE_2D, gl_shadow->texture_id());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    shadow_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* shadow_fbo_ptr = ctx_->framebuffer(shadow_fbo_);
    if (!shadow_fbo_.is_valid() || !shadow_fbo_ptr ||
        !shadow_fbo_ptr->create(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("Renderer2D: failed to create shadow framebuffer");
        return false;
    }
    shadow_fbo_ptr->attach_depth_texture(shadow_map_ptr);
    if (!shadow_fbo_ptr->is_complete()) {
        GLOG_ERROR("Renderer2D: shadow framebuffer incomplete");
        return false;
    }
    return true;
}

void Renderer2D::destroy_shadow_map() {
    if (shadow_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(shadow_fbo_);
        shadow_fbo_ = RHIFramebufferHandle{};
    }
    if (shadow_map_.is_valid()) {
        ctx_->destroy_texture(shadow_map_);
        shadow_map_ = RHITextureHandle{};
    }
}

bool Renderer2D::create_bloom_targets() {
    if (!ctx_ || screen_width_ <= 0.0f || screen_height_ <= 0.0f) {
        bloom_initialized_ = false;
        return false;
    }

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);

    auto create_fb = [&](RHIFramebufferHandle& fb, RHITextureHandle& tex, bool hdr) -> bool {
        tex = ctx_->create_texture();
        ITexture* tex_ptr = ctx_->texture(tex);
        if (!tex.is_valid() || !tex_ptr) return false;
        TextureFormat fmt = hdr ? TextureFormat::RGBA16F : TextureFormat::RGBA8;
        if (!tex_ptr->create(fmt, w, h, nullptr)) return false;
        tex_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb = ctx_->create_framebuffer();
        IFramebuffer* fb_ptr = ctx_->framebuffer(fb);
        if (!fb.is_valid() || !fb_ptr || !fb_ptr->create(w, h)) return false;
        fb_ptr->attach_color_texture(tex_ptr);
        return fb_ptr->is_complete();
    };

    bool ok = create_fb(scene_fbo_, scene_texture_, true) &&
              create_fb(bloom_fbo_a_, bloom_texture_a_, false) &&
              create_fb(bloom_fbo_b_, bloom_texture_b_, false);

    if (!ok) {
        GLOG_ERROR("Renderer2D: failed to create bloom targets");
        destroy_bloom_targets();
        bloom_initialized_ = false;
        return false;
    }

    bloom_initialized_ = true;
    return true;
}

void Renderer2D::destroy_bloom_targets() {
    auto destroy_fb_tex = [&](RHIFramebufferHandle& fb, RHITextureHandle& tex) {
        if (fb.is_valid()) {
            ctx_->destroy_framebuffer(fb);
            fb = RHIFramebufferHandle{};
        }
        if (tex.is_valid()) {
            ctx_->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    };
    destroy_fb_tex(scene_fbo_, scene_texture_);
    destroy_fb_tex(bloom_fbo_a_, bloom_texture_a_);
    destroy_fb_tex(bloom_fbo_b_, bloom_texture_b_);
    bloom_initialized_ = false;
}

void Renderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    lit_batches_.clear();
    shadow_caster_vertices_.clear();
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

    // 窗口尺寸变化时重建 bloom 中间目标
    if (bloom_params_.enabled && bloom_initialized_) {
        ITexture* scene_tex = ctx_->texture(scene_texture_);
        bool need_recreate = !scene_tex ||
                             static_cast<int>(scene_tex->width()) != static_cast<int>(screen_width_) ||
                             static_cast<int>(scene_tex->height()) != static_cast<int>(screen_height_);
        if (need_recreate) {
            destroy_bloom_targets();
            create_bloom_targets();
        }
    }
}

void Renderer2D::set_camera(const math::Vector2f& center, float zoom) {
    camera_center_ = center;
    camera_zoom_ = zoom <= 0.0f ? 1.0f : zoom;
    if (screen_width_ > 0.0f && screen_height_ > 0.0f) {
        math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
        math::Matrix4f view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                            * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                            * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        view_proj_ = ortho_ * view;
    }
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

    bool use_bloom = bloom_params_.enabled && bloom_initialized_;

    if (use_bloom) {
        // 将整个 2D 场景渲染到 HDR 中间纹理
        ctx_->push_command([this](IRenderBackend* backend) {
            backend->bind_framebuffer(scene_fbo_);
            backend->set_viewport(0, 0, static_cast<int>(screen_width_), static_cast<int>(screen_height_));
            backend->clear(0.0f, 0.0f, 0.0f, 0.0f);
            backend->set_depth_test(false);
            backend->set_blend(true);
            backend->set_cull_face(false);
        });
    }

    // 1. 渲染不受光照的 2D 几何体
    flush_batches();

    // 2. 阴影 pass
    render_shadow_pass();

    // 3. 前向光照
    if (!lit_batches_.empty() && context_alive() && lit_sprite_shader_.is_valid()) {
        render_lit_sprites_forward(use_bloom);
    }

    // 4. Bloom 后处理
    if (use_bloom) {
        render_bloom_pass();
    }
}

void Renderer2D::render_shadow_pass() {
    if (!ctx_ || shadow_caster_vertices_.empty() || !shadow_shader_.is_valid() || !shadow_fbo_.is_valid()) {
        return;
    }

    // 找到第一个投射阴影的光源（Directional 优先，否则 Spot）
    const Light2D* shadow_light = nullptr;
    int shadow_light_index = -1;
    for (int i = 0; i < static_cast<int>(lights_.size()); ++i) {
        if (lights_[i].type == LightType2D::Directional || lights_[i].type == LightType2D::Spot) {
            shadow_light = &lights_[i];
            shadow_light_index = i;
            break;
        }
    }
    if (!shadow_light) return;

    math::Vector2f dir = shadow_light->direction.normalized();
    if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);

    // 构建光源空间矩阵：以摄像机为中心，沿光源方向观察
    math::Vector2f center = camera_center_;
    float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
    math::Vector2f eye = center - dir * view_size;
    math::Vector3f eye3(eye.x, eye.y, 0.0f);
    math::Vector3f center3(center.x, center.y, 0.0f);
    math::Vector3f up3(0.0f, 0.0f, 1.0f);
    math::Matrix4f light_view = math::Matrix4f::look_at(eye3, center3, up3);
    math::Matrix4f light_proj;
    if (shadow_light->type == LightType2D::Directional) {
        light_proj = math::Matrix4f::ortho(-view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
    } else {
        // Spot：简化为正交投影（2D 中透视阴影效果不明显，正交更易稳定）
        light_proj = math::Matrix4f::ortho(-view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
    }
    math::Matrix4f light_space = light_proj * light_view;

    auto verts_shared = std::make_shared<std::vector<ShadowCasterVertex>>(std::move(shadow_caster_vertices_));
    shadow_caster_vertices_.clear();

    ctx_->push_command([this, verts_shared, light_space](IRenderBackend* backend) {
        IMesh* mesh_ptr = ctx_->mesh(mesh_);
        IShader* shader_ptr = ctx_->shader(shadow_shader_);
        IFramebuffer* shadow_fbo_ptr = ctx_->framebuffer(shadow_fbo_);
        if (!mesh_ptr || !shader_ptr || !shadow_fbo_ptr) return;

        backend->bind_framebuffer(shadow_fbo_);
        backend->set_viewport(0, 0, k_shadow_map_size, k_shadow_map_size);
        // 阴影 FBO 仅有深度附件，需显式禁用颜色读写。
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        backend->clear_depth();
        backend->set_depth_test(true);
        backend->set_blend(false);
        backend->set_cull_face(false);

        mesh_ptr->upload_vertices(verts_shared->data(),
                                  static_cast<uint32_t>(verts_shared->size() * sizeof(ShadowCasterVertex)),
                                  static_cast<uint32_t>(verts_shared->size()));

        VertexLayout layout;
        layout.stride = sizeof(ShadowCasterVertex);
        layout.attributes = {{0, VertexType::Float2, false, 0}};
        mesh_ptr->set_layout(layout);

        shader_ptr->bind();
        shader_ptr->set_mat4("uLightSpaceMatrix", light_space);
        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::render_lit_sprites_forward(bool target_is_scene_fbo) {
    if (!ctx_ || lit_batches_.empty()) return;

    auto batches_copy = std::make_shared<std::vector<LitBatch>>();
    batches_copy->reserve(lit_batches_.size());
    for (auto& batch : lit_batches_) {
        batches_copy->push_back({batch.albedo, batch.normal,
                                 std::vector<LitVertex2D>(batch.verts)});
    }
    lit_batches_.clear();

    math::Matrix4f view_proj = view_proj_;
    Color ambient = ambient_light_;
    auto lights_copy = std::make_shared<std::vector<Light2D>>(lights_);

    // 找到投射阴影的光源索引
    int shadow_light_index = -1;
    for (int i = 0; i < static_cast<int>(lights_copy->size()); ++i) {
        if ((*lights_copy)[i].type == LightType2D::Directional || (*lights_copy)[i].type == LightType2D::Spot) {
            shadow_light_index = i;
            break;
        }
    }

    // 构建光源空间矩阵（与 shadow pass 一致）
    math::Matrix4f light_space = math::Matrix4f::identity();
    bool use_shadow = false;
    if (shadow_light_index >= 0 && shadow_fbo_.is_valid()) {
        const Light2D& shadow_light = (*lights_copy)[shadow_light_index];
        math::Vector2f dir = shadow_light.direction.normalized();
        if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);
        math::Vector2f center = camera_center_;
        float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
        math::Vector2f eye = center - dir * view_size;
        math::Matrix4f light_view = math::Matrix4f::look_at(
            math::Vector3f(eye.x, eye.y, 0.0f),
            math::Vector3f(center.x, center.y, 0.0f),
            math::Vector3f(0.0f, 0.0f, 1.0f));
        math::Matrix4f light_proj = math::Matrix4f::ortho(
            -view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
        light_space = light_proj * light_view;
        use_shadow = true;
    }

    ctx_->push_command([this, batches_copy, view_proj, ambient, lights_copy,
                        shadow_light_index, light_space, use_shadow, target_is_scene_fbo](IRenderBackend* backend) {
        if (batches_copy->empty()) return;

        IMesh* mesh_ptr = ctx_->mesh(mesh_);
        IShader* shader_ptr = ctx_->shader(lit_sprite_shader_);
        if (!mesh_ptr || !shader_ptr) return;

        // 阴影 pass 会切换 FBO，光照 pass 需要重新绑定到正确目标（scene FBO 或默认 backbuffer）。
        backend->bind_framebuffer(target_is_scene_fbo ? scene_fbo_ : RHIFramebufferHandle{});
        backend->set_viewport(0, 0, static_cast<int>(screen_width_), static_cast<int>(screen_height_));
        backend->set_depth_test(false);
        backend->set_blend(true);
        backend->set_cull_face(false);

        // 创建默认 fallback 纹理
        static GLuint default_albedo_tex = 0;
        static GLuint default_normal_tex = 0;
        if (default_albedo_tex == 0) {
            glGenTextures(1, &default_albedo_tex);
            glBindTexture(GL_TEXTURE_2D, default_albedo_tex);
            unsigned char white[] = {255, 255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (default_normal_tex == 0) {
            glGenTextures(1, &default_normal_tex);
            glBindTexture(GL_TEXTURE_2D, default_normal_tex);
            unsigned char blue[] = {128, 128, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        shader_ptr->bind();
        shader_ptr->set_mat4("uViewProj", view_proj);
        shader_ptr->set_vec3("uAmbientLight", math::Vector3f(ambient.r, ambient.g, ambient.b));

        int light_count = static_cast<int>(std::min<size_t>(lights_copy->size(), 32));
        shader_ptr->set_int("uLightCount", light_count);
        for (int i = 0; i < light_count; ++i) {
            const auto& L = (*lights_copy)[i];
            shader_ptr->set_int(("uLightType[" + std::to_string(i) + "]").c_str(), static_cast<int>(L.type));
            shader_ptr->set_vec2(("uLightPos[" + std::to_string(i) + "]").c_str(), L.position);
            shader_ptr->set_vec2(("uLightDir[" + std::to_string(i) + "]").c_str(), L.direction);
            shader_ptr->set_vec3(("uLightColor[" + std::to_string(i) + "]").c_str(),
                                 math::Vector3f(L.color.r, L.color.g, L.color.b));
            shader_ptr->set_float(("uLightIntensity[" + std::to_string(i) + "]").c_str(), L.intensity);
            shader_ptr->set_float(("uLightRadius[" + std::to_string(i) + "]").c_str(), L.radius);
            shader_ptr->set_float(("uLightRange[" + std::to_string(i) + "]").c_str(), L.range);
            shader_ptr->set_float(("uLightSpotAngle[" + std::to_string(i) + "]").c_str(), L.spot_angle);
            shader_ptr->set_float(("uLightSpotSoftness[" + std::to_string(i) + "]").c_str(), L.spot_softness);
        }

        shader_ptr->set_int("uUseShadowMap", use_shadow ? 1 : 0);
        shader_ptr->set_int("uShadowLightIndex", shadow_light_index);
        if (use_shadow) {
            shader_ptr->set_mat4("uLightSpaceMatrix", light_space);
            ITexture* shadow_map_ptr = ctx_->texture(shadow_map_);
            if (shadow_map_ptr) shadow_map_ptr->bind(2);
            shader_ptr->set_int("uShadowMap", 2);
        }

        for (const auto& batch : *batches_copy) {
            if (batch.verts.empty()) continue;

            if (batch.albedo) {
                batch.albedo->bind(0);
            } else {
                glBindTextureUnit(0, default_albedo_tex);
            }
            shader_ptr->set_int("uAlbedo", 0);

            if (batch.normal) {
                batch.normal->bind(1);
            } else {
                glBindTextureUnit(1, default_normal_tex);
            }
            shader_ptr->set_int("uNormalMap", 1);

            mesh_ptr->upload_vertices(batch.verts.data(),
                                      static_cast<uint32_t>(batch.verts.size() * sizeof(LitVertex2D)),
                                      static_cast<uint32_t>(batch.verts.size()));

            VertexLayout layout;
            layout.stride = sizeof(LitVertex2D);
            layout.attributes = {
                {0, VertexType::Float2, false, 0},
                {1, VertexType::Float4, false, 2 * sizeof(float)},
                {2, VertexType::Float2, false, 6 * sizeof(float)},
                {3, VertexType::Float2, false, 8 * sizeof(float)}
            };
            mesh_ptr->set_layout(layout);
            mesh_ptr->draw();
        }

        shader_ptr->unbind();
    });
}

void Renderer2D::render_bloom_pass() {
    if (!ctx_ || !bloom_initialized_) return;

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);

    auto do_fullscreen_pass = [&](RHIShaderHandle shader, RHIFramebufferHandle target,
                                   RHITextureHandle input_tex, auto set_uniforms) {
        ctx_->push_command([this, shader, target, input_tex, set_uniforms, w, h](IRenderBackend* backend) {
            IMesh* mesh_ptr = ctx_->mesh(mesh_);
            IShader* shader_ptr = ctx_->shader(shader);
            ITexture* input_ptr = ctx_->texture(input_tex);
            if (!mesh_ptr || !shader_ptr) return;

            backend->bind_framebuffer(target);
            backend->set_viewport(0, 0, w, h);
            backend->set_depth_test(false);
            backend->set_blend(false);
            backend->set_cull_face(false);

            // 全屏三角形
            struct FSVertex { float x, y, u, v; };
            FSVertex verts[] = {
                {-1.0f, -1.0f, 0.0f, 0.0f},
                { 3.0f, -1.0f, 2.0f, 0.0f},
                {-1.0f,  3.0f, 0.0f, 2.0f}
            };
            mesh_ptr->upload_vertices(verts, sizeof(verts), 3);
            VertexLayout layout;
            layout.stride = sizeof(FSVertex);
            layout.attributes = {
                {0, VertexType::Float2, false, 0},
                {1, VertexType::Float2, false, 2 * sizeof(float)}
            };
            mesh_ptr->set_layout(layout);

            shader_ptr->bind();
            if (input_ptr) input_ptr->bind(0);
            shader_ptr->set_int("uTexture", 0);
            set_uniforms(shader_ptr);
            mesh_ptr->draw();
            shader_ptr->unbind();
        });
    };

    // Threshold
    do_fullscreen_pass(bloom_threshold_shader_, bloom_fbo_a_, scene_texture_,
                       [&](IShader* s) { s->set_float("uThreshold", bloom_params_.threshold); });

    // Blur passes
    RHITextureHandle src = bloom_texture_a_;
    RHIFramebufferHandle dst = bloom_fbo_b_;
    for (int i = 0; i < bloom_params_.blur_passes * 2; ++i) {
        bool horizontal = (i % 2) == 0;
        do_fullscreen_pass(bloom_blur_shader_, dst, src,
                           [&](IShader* s) {
                               s->set_vec2("uDirection", horizontal ? math::Vector2f(1.0f, 0.0f) : math::Vector2f(0.0f, 1.0f));
                               s->set_vec2("uTexelSize", math::Vector2f(1.0f / w, 1.0f / h));
                           });
        std::swap(src, bloom_texture_a_);
        std::swap(dst, bloom_fbo_a_);
    }
    // 最终 bloom 结果在 src 中

    // Compose to backbuffer
    ctx_->push_command([this, src, w, h](IRenderBackend* backend) {
        IMesh* mesh_ptr = ctx_->mesh(mesh_);
        IShader* shader_ptr = ctx_->shader(bloom_compose_shader_);
        ITexture* scene_ptr = ctx_->texture(scene_texture_);
        ITexture* bloom_ptr = ctx_->texture(src);
        if (!mesh_ptr || !shader_ptr) return;

        backend->bind_framebuffer(RHIFramebufferHandle{});
        backend->set_viewport(0, 0, w, h);
        backend->set_depth_test(false);
        backend->set_blend(false);
        backend->set_cull_face(false);

        struct FSVertex { float x, y, u, v; };
        FSVertex verts[] = {
            {-1.0f, -1.0f, 0.0f, 0.0f},
            { 3.0f, -1.0f, 2.0f, 0.0f},
            {-1.0f,  3.0f, 0.0f, 2.0f}
        };
        mesh_ptr->upload_vertices(verts, sizeof(verts), 3);
        VertexLayout layout;
        layout.stride = sizeof(FSVertex);
        layout.attributes = {
            {0, VertexType::Float2, false, 0},
            {1, VertexType::Float2, false, 2 * sizeof(float)}
        };
        mesh_ptr->set_layout(layout);

        shader_ptr->bind();
        if (scene_ptr) scene_ptr->bind(0);
        shader_ptr->set_int("uScene", 0);
        if (bloom_ptr) bloom_ptr->bind(1);
        shader_ptr->set_int("uBloom", 1);
        shader_ptr->set_float("uIntensity", bloom_params_.intensity);
        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::push_vertex(float x, float y, const Color& color, float u, float v) {
    vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void Renderer2D::push_text_vertex(float x, float y, const Color& color, float u, float v) {
    text_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void Renderer2D::push_lit_vertex(ITexture* albedo, ITexture* normal,
                                 float x, float y, const Color& color,
                                 float u, float v, float nu, float nv) {
    auto it = find_lit_batch(albedo, normal);
    it->verts.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
}

void Renderer2D::push_shadow_caster_vertex(float x, float y) {
    shadow_caster_vertices_.push_back({x, y});
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

        mesh_ptr->upload_vertices(verts_shared->data(),
                                  static_cast<uint32_t>(verts_shared->size() * sizeof(Vertex2D)),
                                  static_cast<uint32_t>(verts_shared->size()));

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

void Renderer2D::add_light(const Light2D& light) {
    if (lights_.size() < 32) {
        lights_.push_back(light);
    }
}

void Renderer2D::reset_lights() {
    lights_.clear();
    ambient_light_ = Color::black();
}

void Renderer2D::set_bloom(const BloomParams& params) {
    bloom_params_ = params;
    if (bloom_params_.enabled && !bloom_initialized_ && screen_width_ > 0.0f && screen_height_ > 0.0f) {
        create_bloom_targets();
    }
}

void Renderer2D::draw_sprite(float x, float y, float w, float h,
                              ITexture* texture, const Color& tint) {
    draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

void Renderer2D::draw_sprite_region(float x, float y, float w, float h,
                                     float u0, float v0, float u1, float v1,
                                     ITexture* texture, const Color& tint) {
    if (!texture || !context_alive() || !mesh_.is_valid() || !shader_.is_valid()) return;

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
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, albedo, normal_map, tint,
                           0.0f, 0.0f, 1.0f, 1.0f);
}

void Renderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                         float u0, float v0, float u1, float v1,
                                         ITexture* albedo, ITexture* normal_map,
                                         const Color& tint,
                                         float nu0, float nv0,
                                         float nu1, float nv1) {
    if (!context_alive()) return;

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y0, tint, u1, v0, nu1, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);
    push_lit_vertex(albedo, normal_map, x0, y1, tint, u0, v1, nu0, nv1);
}

void Renderer2D::draw_shadow_caster(float x, float y, float w, float h) {
    if (!context_alive()) return;
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y1);
}

RHITextureHandle Renderer2D::create_texture_from_data(const assets::TextureData* data) {
    if (!data || data->empty() || !context_alive() || !ctx_) return RHITextureHandle{};

    RHITextureHandle tex = ctx_->create_texture();
    if (!tex.is_valid()) return RHITextureHandle{};

    auto pixels_copy = std::make_shared<std::vector<unsigned char>>(data->pixels);
    int width = data->width;
    int height = data->height;
    int channels = data->channels;

    ctx_->push_command([this, tex, pixels_copy, width, height, channels](IRenderBackend*) {
        ITexture* tex_ptr = ctx_->texture(tex);
        if (!tex_ptr) return;
        tex_ptr->upload_data(pixels_copy->data(), width, height, channels);
        tex_ptr->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    });

    return tex;
}

ITexture* Renderer2D::resolve_texture(RHITextureHandle handle) const {
    return ctx_ ? ctx_->texture(handle) : nullptr;
}

} // namespace gryce_engine::render
