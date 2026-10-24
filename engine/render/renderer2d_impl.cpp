#include "renderer2d_impl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <GL/glew.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "render/render_context.h"
#include "render/render_commands.h"
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

// 预先生成光照 uniform 名称，避免每帧构造字符串。
struct LightUniformNames {
    std::array<std::string, 32> type;
    std::array<std::string, 32> pos;
    std::array<std::string, 32> dir;
    std::array<std::string, 32> color;
    std::array<std::string, 32> intensity;
    std::array<std::string, 32> radius;
    std::array<std::string, 32> range;
    std::array<std::string, 32> spot_angle;
    std::array<std::string, 32> spot_softness;

    LightUniformNames() {
        for (int i = 0; i < 32; ++i) {
            type[i] = "uLightType[" + std::to_string(i) + "]";
            pos[i] = "uLightPos[" + std::to_string(i) + "]";
            dir[i] = "uLightDir[" + std::to_string(i) + "]";
            color[i] = "uLightColor[" + std::to_string(i) + "]";
            intensity[i] = "uLightIntensity[" + std::to_string(i) + "]";
            radius[i] = "uLightRadius[" + std::to_string(i) + "]";
            range[i] = "uLightRange[" + std::to_string(i) + "]";
            spot_angle[i] = "uLightSpotAngle[" + std::to_string(i) + "]";
            spot_softness[i] = "uLightSpotSoftness[" + std::to_string(i) + "]";
        }
    }
};
static const LightUniformNames k_light_uniform_names;

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
        // 字形覆盖度直接当 alpha 用，**不做阈值化**：smoothstep(0.45,0.55,a)
        // 会把 8bit 覆盖度压成 0/1 两档，字缘全是硬阶梯（实质等于没有抗锯齿）。
        // 抗锯齿的灰度信息是 stb_truetype 逐像素算出来的，混掉它等于白烘一遍。
        float a = texture(uTexture, vTexCoord).a * vColor.a;
        if (a <= 0.0) discard;
        FragColor = vec4(vColor.rgb, a);
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
            if (font_atlas_.init(ctx_, bundled, text_raster_hint_)) {
                using_fallback_font_ = false;
                font_path_ = bundled;
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
            "micross.ttf",
            "DejaVuSans.ttf",
            "NotoSans-Regular.ttf",
            nullptr
        };

        for (int i = 0; font_names[i]; ++i) {
            std::string font_path = font_dir + font_names[i];
            if (font_atlas_.init(ctx_, font_path, text_raster_hint_)) {
                using_fallback_font_ = false;
                font_path_ = font_path;
                GLOG_INFO("Renderer2D: loaded system font '{}'", font_path);
                break;
            }
        }
    }

    // 候选名全失败（精简版 Windows / 容器镜像的字体目录往往只有一两个文件）：
    // 直接扫目录取第一个能解析的 TTF/TTC，避免必然掉进"实心色块"回退。
    if (!font_atlas_.texture()) {
        std::error_code ec;
        const std::string font_dir = get_system_font_dir();
        std::vector<std::string> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(font_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (ext == ".ttf" || ext == ".ttc" || ext == ".otf") {
                candidates.push_back(entry.path().string());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (const std::string& path : candidates) {
            if (font_atlas_.init(ctx_, path, text_raster_hint_)) {
                using_fallback_font_ = false;
                font_path_ = path;
                GLOG_INFO("Renderer2D: loaded scanned system font '{}'", path);
                break;
            }
        }
        if (!font_atlas_.texture() && !candidates.empty()) {
            GLOG_WARN("Renderer2D: {} font file(s) found in '{}' but none could be parsed",
                      candidates.size(), font_dir);
        }
    }

    if (!font_atlas_.texture()) {
        GLOG_WARN("Renderer2D: no system font loaded, creating fallback atlas");
        if (font_atlas_.create_fallback_atlas(ctx_, text_raster_hint_)) {
            using_fallback_font_ = true;
        } else {
            GLOG_ERROR("Renderer2D: failed to create fallback font atlas, text rendering disabled");
        }
    }

    // 除界面文字那一档，再固定烘一档 32px 通用尺寸：引擎自带的叠加层文字
    // （FPS / 帧耗时，22px）与 2D Label 组件用的是任意字号，只有 13px 一档时
    // 它们会被放大 1.7 倍（比原来从 32 缩下来还糊）。两张档同在一张图集里，
    // 选档在 draw_text 里按相对误差做，调用方不用关心。
    if (!using_fallback_font_ && std::fabs(font_atlas_.font_size() - 32.0f) >= 1.0f) {
        font_atlas_.ensure_size(ctx_, 32.0f);
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
        // 先提交并等待渲染线程执行完所有已排队命令（这些命令大多捕获了 this），
        // 确保本对象销毁后不会有残留命令在渲染线程访问悬垂的 this。
        // 仅在渲染线程运行中执行；暂停状态下 cmd_buffer_ 可能已为空指针。
        if (ctx_->is_running()) {
            // 默认 albedo/normal 纹理在渲染线程（lit 前向命令内）创建，须在同一线程删除。
            // 该命令排在所有已排队的 lit 命令之后执行，配合 present+wait_for_idle
            // 保证执行完毕前本对象不被销毁。
            ctx_->push_command([this](IRenderBackend*) {
                if (default_albedo_tex_ != 0) {
                    destroy_default_texture(default_albedo_tex_);
                    default_albedo_tex_ = 0;
                }
                if (default_normal_tex_ != 0) {
                    destroy_default_texture(default_normal_tex_);
                    default_normal_tex_ = 0;
                }
            });
            ctx_->present();
            ctx_->wait_for_idle();
        } else {
            // 渲染线程未运行（暂停中）：context 已切回主线程，可直接删除
            if (default_albedo_tex_ != 0) {
                destroy_default_texture(default_albedo_tex_);
                default_albedo_tex_ = 0;
            }
            if (default_normal_tex_ != 0) {
                destroy_default_texture(default_normal_tex_);
                default_normal_tex_ = 0;
            }
        }
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
    configure_depth_texture_for_sampling(shadow_map_ptr);

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

    math::Matrix4f view;
    if (camera_top_left_origin_) {
        view = math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    } else {
        math::Vector2f screen_center(screen_width * 0.5f, screen_height * 0.5f);
        view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
             * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
             * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
             * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    }
    view_proj_ = ortho_ * view;
    if (context_alive()) {
        // 重置视口为全屏，防止 3D 管线残留的视口设置影响 UI 绘制区域
        ctx_->set_viewport(0, 0, static_cast<int>(screen_width), static_cast<int>(screen_height));
        ctx_->set_depth_test(false);
        ctx_->set_blend(true);
        ctx_->set_cull_face(CullMode::None);
        set_blend_mode(BlendMode::Alpha);
    }

    // 窗口尺寸变化时重建 bloom 中间目标
    if (bloom_params_.enabled && bloom_initialized_) {
        ITexture* scene_tex = ctx_->texture(scene_texture_);
        bool need_recreate = !scene_tex ||
                             static_cast<int>(scene_tex->width()) != static_cast<int>(screen_width_) ||
                             static_cast<int>(scene_tex->height()) != static_cast<int>(screen_height_);
        if (need_recreate) {
            // 销毁/创建含直接 GL 调用（glCreateTextures/glCreateFramebuffers 等），
            // 渲染线程运行中主线程无 current context：
            // 先暂停渲染线程（context 切回主线程）再重建，完成后恢复。
            if (ctx_->is_running()) {
                ctx_->pause_render_thread();
                destroy_bloom_targets();
                create_bloom_targets();
                ctx_->resume_render_thread();
            } else {
                destroy_bloom_targets();
                create_bloom_targets();
            }
        }
    }
}

void Renderer2D::set_camera(const math::Vector2f& center, float zoom, bool top_left_origin,
                            float rotation) {
    // 切换摄像机前必须先提交当前已缓冲的顶点，确保它们使用绘制时的 view_proj 矩阵，
    // 否则 UI 层（top_left_origin）顶点会在后续 flush 时被普通摄像机矩阵变换到屏幕中心。
    flush_batches();

    camera_center_ = center;
    camera_zoom_ = zoom <= 0.0f ? 1.0f : zoom;
    camera_rotation_ = rotation;
    camera_top_left_origin_ = top_left_origin;
    if (screen_width_ > 0.0f && screen_height_ > 0.0f) {
        math::Matrix4f view;
        if (top_left_origin) {
            view = math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                 * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
                 * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        } else {
            math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
            view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                 * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                 * math::Matrix4f::rotate(-camera_rotation_, math::Vector3f(0.0f, 0.0f, 1.0f))
                 * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        }
        view_proj_ = ortho_ * view;
    }
}

math::Vector2f Renderer2D::world_to_screen(const math::Vector2f& world) const {
    const float c = std::cos(-camera_rotation_);
    const float s = std::sin(-camera_rotation_);
    const math::Vector2f d = world - camera_center_;
    const math::Vector2f rotated(d.x * c - d.y * s, d.x * s + d.y * c);
    if (camera_top_left_origin_) {
        return rotated * camera_zoom_;
    }
    math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
    return screen_center + rotated * camera_zoom_;
}

void Renderer2D::end_frame() {
    if (context_alive()) {
        ctx_->set_depth_test(false);
        ctx_->set_blend(true);
        ctx_->set_cull_face(CullMode::None);
    }

    bool use_bloom = bloom_params_.enabled && bloom_initialized_;

    if (use_bloom) {
        // 将整个 2D 场景渲染到 HDR 中间纹理。
        // 按值拷贝命令所需状态，渲染线程只读命令内的拷贝，避免与主线程成员写竞争。
        const RHIFramebufferHandle scene_fbo = scene_fbo_;
        const int sw = static_cast<int>(screen_width_);
        const int sh = static_cast<int>(screen_height_);
        ctx_->push_command([this, scene_fbo, sw, sh](IRenderBackend* backend) {
            if (use_backend_framebuffers()) backend->bind_framebuffer(scene_fbo);
            backend->set_viewport(0, 0, sw, sh);
            backend->clear(0.0f, 0.0f, 0.0f, 0.0f);
            backend->set_depth_test(false);
            backend->set_blend(true);
            backend->set_cull_face(CullMode::None);
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

void Renderer2D::set_blend_mode(BlendMode mode) {
    if (mode == blend_mode_) return;

    // 先提交当前按旧混合模式收集的批次，避免同批顶点被错误混合。
    flush_batch(std::move(vertices_), false);

    if (context_alive()) {
        if (mode == BlendMode::Alpha) {
            ctx_->push_command([](IRenderBackend* backend) {
                backend->set_blend(true);
                backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
                backend->set_blend_equation(BlendEquation::Add);
            });
        } else {
            ctx_->push_command([](IRenderBackend* backend) {
                backend->set_blend(true);
                backend->set_blend_func(BlendFactor::SrcAlpha, BlendFactor::One);
                backend->set_blend_equation(BlendEquation::Add);
            });
        }
    }

    blend_mode_ = mode;
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

    // 先判零向量再 normalize：对零向量先 normalized() 会得到 NaN，
    // 而 NaN 的 length_sq() 比较恒为 false，防护会失效。
    math::Vector2f dir = shadow_light->direction.length_sq() < 1e-6f
                         ? math::Vector2f(0.0f, -1.0f)
                         : shadow_light->direction.normalized();

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

        if (use_backend_framebuffers()) backend->bind_framebuffer(shadow_fbo_);
        backend->set_viewport(0, 0, k_shadow_map_size, k_shadow_map_size);
        // 阴影 FBO 仅有深度附件，需显式禁用颜色读写。
        disable_color_writes_for_depth_only_target();
        backend->clear_depth();
        backend->set_depth_test(true);
        backend->set_blend(false);
        backend->set_cull_face(CullMode::None);

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

    std::vector<LitBatch> batches_copy;
    batches_copy.reserve(lit_batches_.size());
    for (auto& kv : lit_batches_) {
        batches_copy.push_back({kv.second.albedo, kv.second.normal,
                                std::move(kv.second.verts), kv.second.view_proj});
    }
    lit_batches_.clear();
    auto batches_shared = std::make_shared<std::vector<LitBatch>>(std::move(batches_copy));

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
        // 先判零向量再 normalize（NaN 比较恒 false，先 normalized() 防护会失效）
        math::Vector2f dir = shadow_light.direction.length_sq() < 1e-6f
                             ? math::Vector2f(0.0f, -1.0f)
                             : shadow_light.direction.normalized();
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

    // 按值拷贝命令所需的成员状态（句柄/尺寸），渲染线程执行时只读命令内拷贝，
    // 避免与主线程对 screen_width_ / bloom 句柄等成员的写操作构成数据竞争。
    const RHIFramebufferHandle scene_fbo = scene_fbo_;
    const RHIMeshHandle mesh = mesh_;
    const RHIShaderHandle lit_shader = lit_sprite_shader_;
    const RHITextureHandle shadow_map = shadow_map_;
    const int sw = static_cast<int>(screen_width_);
    const int sh = static_cast<int>(screen_height_);

    ctx_->push_command([this, batches_shared, ambient, lights_copy,
                        shadow_light_index, light_space, use_shadow, target_is_scene_fbo,
                        scene_fbo, mesh, lit_shader, shadow_map, sw, sh](IRenderBackend* backend) {
        if (batches_shared->empty()) return;

        IMesh* mesh_ptr = ctx_->mesh(mesh);
        IShader* shader_ptr = ctx_->shader(lit_shader);
        if (!mesh_ptr || !shader_ptr) return;

        // 阴影 pass 会切换 FBO，光照 pass 需要重新绑定到正确目标（scene FBO 或默认 backbuffer）。
        if (use_backend_framebuffers()) {
            backend->bind_framebuffer(target_is_scene_fbo ? scene_fbo : RHIFramebufferHandle{});
        }
        backend->set_viewport(0, 0, sw, sh);
        backend->set_depth_test(false);
        backend->set_blend(true);
        backend->set_cull_face(CullMode::None);

        // 创建默认 fallback 纹理（成员变量，仅渲染线程访问；
        // shutdown 时通过命令在同一线程删除，避免函数级 static 造成进程级泄漏）
        if (default_albedo_tex_ == 0) {
            const unsigned char white[] = {255, 255, 255, 255};
            default_albedo_tex_ = create_default_1x1_texture(white);
        }
        if (default_normal_tex_ == 0) {
            const unsigned char blue[] = {128, 128, 255, 255};
            default_normal_tex_ = create_default_1x1_texture(blue);
        }

        shader_ptr->bind();
        shader_ptr->set_vec3("uAmbientLight", math::Vector3f(ambient.r, ambient.g, ambient.b));

        int light_count = static_cast<int>(std::min<size_t>(lights_copy->size(), 32));
        shader_ptr->set_int("uLightCount", light_count);
        for (int i = 0; i < light_count; ++i) {
            const auto& L = (*lights_copy)[i];
            shader_ptr->set_int(k_light_uniform_names.type[i].c_str(), static_cast<int>(L.type));
            shader_ptr->set_vec2(k_light_uniform_names.pos[i].c_str(), L.position);
            shader_ptr->set_vec2(k_light_uniform_names.dir[i].c_str(), L.direction);
            shader_ptr->set_vec3(k_light_uniform_names.color[i].c_str(),
                                 math::Vector3f(L.color.r, L.color.g, L.color.b));
            shader_ptr->set_float(k_light_uniform_names.intensity[i].c_str(), L.intensity);
            shader_ptr->set_float(k_light_uniform_names.radius[i].c_str(), L.radius);
            shader_ptr->set_float(k_light_uniform_names.range[i].c_str(), L.range);
            shader_ptr->set_float(k_light_uniform_names.spot_angle[i].c_str(), L.spot_angle);
            shader_ptr->set_float(k_light_uniform_names.spot_softness[i].c_str(), L.spot_softness);
        }

        shader_ptr->set_int("uUseShadowMap", use_shadow ? 1 : 0);
        shader_ptr->set_int("uShadowLightIndex", shadow_light_index);
        if (use_shadow) {
            shader_ptr->set_mat4("uLightSpaceMatrix", light_space);
            // 2D shadow map 固定到 TextureSlots::k2DShadow，避免与 3D PBR 的
            // roughness/metallic/ao 产生 depth-vs-color 纹理类型冲突，触发 NVIDIA shader recompile。
            ITexture* shadow_map_ptr = ctx_->texture(shadow_map);
            // 原始深度读取：lit shader 在 CPU 侧手动比较深度，需要关闭纹理
            // 对象上的 GL_TEXTURE_COMPARE_MODE（否则 sampler2D 取到 0/1 比较结果，
            // 阴影覆盖范围内全部判为遮挡、整片变黑）。
            if (shadow_map_ptr) shadow_map_ptr->bind_raw_depth(TextureSlots::k2DShadow);
            shader_ptr->set_int("uShadowMap", TextureSlots::k2DShadow);
        }

        for (const auto& batch : *batches_shared) {
            if (batch.verts.empty()) continue;

            shader_ptr->set_mat4("uViewProj", batch.view_proj);

            // 句柄在执行时经 generation 校验解析；纹理已销毁则回退默认纹理
            if (ITexture* albedo_ptr = ctx_->texture(batch.albedo)) {
                albedo_ptr->bind(TextureSlots::k2DAlbedo);
            } else {
                bind_default_texture_slot(TextureSlots::k2DAlbedo, default_albedo_tex_);
            }
            shader_ptr->set_int("uAlbedo", TextureSlots::k2DAlbedo);

            if (ITexture* normal_ptr = ctx_->texture(batch.normal)) {
                normal_ptr->bind(TextureSlots::k2DNormal);
            } else {
                bind_default_texture_slot(TextureSlots::k2DNormal, default_normal_tex_);
            }
            shader_ptr->set_int("uNormalMap", TextureSlots::k2DNormal);

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
    // 按值拷贝 mesh 句柄，渲染线程只读命令内拷贝
    const RHIMeshHandle mesh = mesh_;

    auto do_fullscreen_pass = [&](RHIShaderHandle shader, RHIFramebufferHandle target,
                                   RHITextureHandle input_tex, auto set_uniforms) {
        ctx_->push_command([this, mesh, shader, target, input_tex, set_uniforms, w, h](IRenderBackend* backend) {
            IMesh* mesh_ptr = ctx_->mesh(mesh);
            IShader* shader_ptr = ctx_->shader(shader);
            ITexture* input_ptr = ctx_->texture(input_tex);
            if (!mesh_ptr || !shader_ptr) return;

            if (use_backend_framebuffers()) backend->bind_framebuffer(target);
            backend->set_viewport(0, 0, w, h);
            backend->set_depth_test(false);
            backend->set_blend(false);
            backend->set_cull_face(CullMode::None);

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

    // Threshold（threshold 按值捕获，避免渲染线程读到主线程正在写的 bloom_params_）
    const float threshold = bloom_params_.threshold;
    do_fullscreen_pass(bloom_threshold_shader_, bloom_fbo_a_, scene_texture_,
                       [threshold](IShader* s) { s->set_float("uThreshold", threshold); });

    // Blur passes：在 a/b 两套目标之间正确 ping-pong（纯局部交替，不交换成员句柄）。
    // 旧实现 std::swap(src, bloom_texture_a_) 首轮即自交换恒无效果，
    // 且会产生“向正在采样的纹理写入”的 FBO 反馈回路。
    // blur_passes*2 恒为偶数，最后一次写入 fbo_a，结果始终落在 bloom_texture_a_。
    for (int i = 0; i < bloom_params_.blur_passes * 2; ++i) {
        const bool horizontal = (i % 2) == 0;
        const bool read_a = (i % 2) == 0;
        const RHITextureHandle src_tex = read_a ? bloom_texture_a_ : bloom_texture_b_;
        const RHIFramebufferHandle dst_fb = read_a ? bloom_fbo_b_ : bloom_fbo_a_;
        do_fullscreen_pass(bloom_blur_shader_, dst_fb, src_tex,
                           [horizontal, w, h](IShader* s) {
                               // 按值捕获循环局部变量，命令延迟执行时栈变量已销毁
                               s->set_vec2("uDirection", horizontal ? math::Vector2f(1.0f, 0.0f) : math::Vector2f(0.0f, 1.0f));
                               s->set_vec2("uTexelSize", math::Vector2f(1.0f / w, 1.0f / h));
                           });
    }
    // 最终 bloom 结果在 bloom_texture_a_ 中

    // Compose to backbuffer（句柄/强度/尺寸全部按值拷进命令）
    const RHITextureHandle bloom_result = bloom_texture_a_;
    const RHITextureHandle scene_tex = scene_texture_;
    const RHIShaderHandle compose_shader = bloom_compose_shader_;
    const float intensity = bloom_params_.intensity;
    ctx_->push_command([this, mesh, bloom_result, scene_tex, compose_shader, intensity, w, h](IRenderBackend* backend) {
        IMesh* mesh_ptr = ctx_->mesh(mesh);
        IShader* shader_ptr = ctx_->shader(compose_shader);
        ITexture* scene_ptr = ctx_->texture(scene_tex);
        ITexture* bloom_ptr = ctx_->texture(bloom_result);
        if (!mesh_ptr || !shader_ptr) return;

        if (use_backend_framebuffers()) backend->bind_framebuffer(RHIFramebufferHandle{});
        backend->set_viewport(0, 0, w, h);
        backend->set_depth_test(false);
        backend->set_blend(false);
        backend->set_cull_face(CullMode::None);

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
        shader_ptr->set_float("uIntensity", intensity);
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

void Renderer2D::push_lit_vertex(RHITextureHandle albedo, RHITextureHandle normal,
                                 float x, float y, const Color& color,
                                 float u, float v, float nu, float nv) {
    auto it = find_lit_batch(albedo, normal);
    it->verts.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
}

void Renderer2D::push_shadow_caster_vertex(float x, float y) {
    shadow_caster_vertices_.push_back({x, y});
}

Renderer2D::LitBatch* Renderer2D::find_lit_batch(RHITextureHandle albedo, RHITextureHandle normal) {
    LitBatchKey key{albedo, normal};
    auto it = lit_batches_.find(key);
    if (it == lit_batches_.end()) {
        LitBatch batch;
        batch.albedo = albedo;
        batch.normal = normal;
        batch.view_proj = view_proj_;
        it = lit_batches_.emplace(key, std::move(batch)).first;
    }
    return &it->second;
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
    // 字体图集纹理保留裸指针：FontAtlas 由 Renderer2D 拥有，生命周期与本对象一致，
    // 且 shutdown() 会先 drain 命令队列再销毁图集，不存在悬垂风险，无需 handle 化。
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

namespace {

// 圆角轮廓上的点：位置 + 单位外法线（沿法线偏移即可做内缩/外扩）。
struct RoundPt {
    float x = 0.0f;
    float y = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
};

constexpr float kPi = 3.14159265358979323846f;

// 半径钳制：相邻两角共享一条边，两半径之和超过边长时按比例整体收缩，
// 否则大圆角会把对边压成负长度，扇形会翻折出自交的黑块。
void shrink_radii(float w, float h, float radii[4]) {
    for (int i = 0; i < 4; ++i) {
        if (!(radii[i] > 0.0f)) radii[i] = 0.0f;
    }
    const float max_r = ((w < h) ? w : h) * 0.5f;
    for (int i = 0; i < 4; ++i) {
        if (radii[i] > max_r) radii[i] = max_r;
    }
    // 四条边各自的约束：T(r0+r1<=w) B(r3+r2<=w) L(r0+r3<=h) R(r1+r2<=h)
    const float pairs[4][2] = {{0, 1}, {3, 2}, {0, 3}, {1, 2}};
    const float limits[4] = {w, w, h, h};
    float scale = 1.0f;
    for (int k = 0; k < 4; ++k) {
        const float sum = radii[static_cast<int>(pairs[k][0])] +
                          radii[static_cast<int>(pairs[k][1])];
        if (sum > limits[k] && sum > 0.0f) {
            const float s = limits[k] / sum;
            if (s < scale) scale = s;
        }
    }
    if (scale < 1.0f) {
        for (int i = 0; i < 4; ++i) radii[i] *= scale;
    }
}

// 每角的分段数：由**基准**半径算，不随 outset 变——内轮廓与外轮廓的顶点数
// 必须相等才能组成环带；按各自半径各算一套会让"内外点数不同"，环带被整段
// 丢掉（症状是阴影或描边只剩外半圈）。
int arc_segments(const float radii[4]) {
    float m = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (radii[i] > m) m = radii[i];
    }
    int seg = static_cast<int>(m * 1.2f) + 2;
    if (seg < 3) seg = 3;
    if (seg > 16) seg = 16;
    return seg;
}

// 外轮廓折线（顺时针，左上原点 +y 向下）。outset > 0 外扩、< 0 内缩：
// 圆角矩形沿法线偏移 d 正好等于"矩形与半径同时加 d"，所以这里统一处理。
// 每角恒定输出 seg+1 个点（半径为 0 时全部落在角点上、法线沿两边插值），
// 这样不同 outset 的轮廓点数一致，环带不会因为点数不齐被丢弃。
void build_round_outline(float x, float y, float w, float h, const float radii_in[4],
                         float outset, int seg, std::vector<RoundPt>& out) {
    out.clear();
    if (w <= 0.0f || h <= 0.0f) return;

    const float x0 = x - outset;
    const float y0 = y - outset;
    const float x1 = x + w + outset;
    const float y1 = y + h + outset;
    if (x1 <= x0 || y1 <= y0) return;

    float r[4] = {radii_in[0], radii_in[1], radii_in[2], radii_in[3]};
    for (int i = 0; i < 4; ++i) {
        r[i] = (r[i] > 0.0f) ? (r[i] + outset) : 0.0f;
        if (r[i] < 0.0f) r[i] = 0.0f;
    }
    shrink_radii(x1 - x0, y1 - y0, r);

    out.reserve(static_cast<size_t>(seg + 1) * 4);

    // 四角的圆弧中心、起始角、直角时的角点与两条边法线
    // (顺序：左上 右上 右下 左下)
    const float cx[4] = {x0 + r[0], x1 - r[1], x1 - r[2], x0 + r[3]};
    const float cy[4] = {y0 + r[0], y0 + r[1], y1 - r[2], y1 - r[3]};
    const float a0[4] = {kPi, kPi * 1.5f, 0.0f, kPi * 0.5f};
    const float px[4] = {x0, x1, x1, x0};
    const float py[4] = {y0, y0, y1, y1};
    const float n0x[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
    const float n0y[4] = {0.0f, -1.0f, 0.0f, 1.0f};
    const float n1x[4] = {0.0f, 1.0f, 0.0f, -1.0f};
    const float n1y[4] = {-1.0f, 0.0f, 1.0f, 0.0f};

    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i <= seg; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(seg);
            const float a = a0[k] + t * (kPi * 0.5f);
            const float c = std::cos(a);
            const float s = std::sin(a);
            if (r[k] > 0.0f) {
                out.push_back({cx[k] + r[k] * c, cy[k] + r[k] * s, c, s});
            } else {
                float nx = n0x[k] + (n1x[k] - n0x[k]) * t;
                float ny = n0y[k] + (n1y[k] - n0y[k]) * t;
                const float len = std::sqrt(nx * nx + ny * ny);
                if (len > 1e-6f) { nx /= len; ny /= len; }
                out.push_back({px[k], py[k], nx, ny});
            }
        }
    }
}

} // namespace

void Renderer2D::draw_round_rect(float x, float y, float w, float h,
                                 const float radii[4], const Color& color) {
    draw_round_rect_gradient(x, y, w, h, radii, color, color);
}

void Renderer2D::draw_round_rect_gradient(float x, float y, float w, float h,
                                          const float radii[4],
                                          const Color& top, const Color& bottom) {
    if (w <= 0.0f || h <= 0.0f) return;

    // 抗锯齿带宽 1 像素：内侧 0.5 -> 外侧 0.5，颜色从中段向外线性淡出。
    // 用顶点色做渐变而不是贴图/着色器，是因为这条路径的批次只认 position +
    // color + uv，顶点色插值天然就是"1 像素宽的软边"。
    constexpr float kAa = 0.5f;

    const int seg = arc_segments(radii);
    std::vector<RoundPt> inner;
    std::vector<RoundPt> outer;
    build_round_outline(x, y, w, h, radii, -kAa, seg, inner);
    if (inner.size() < 3) return;
    build_round_outline(x, y, w, h, radii, kAa, seg, outer);

    // 渐变按顶点 y 在 [y, y+h] 上插值（超出范围的顶点按端点夹取）
    const float inv_h = (h > 0.0f) ? (1.0f / h) : 0.0f;
    auto color_at = [&](float py) {
        float t = (py - y) * inv_h;
        t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
        return Color(top.r + (bottom.r - top.r) * t,
                     top.g + (bottom.g - top.g) * t,
                     top.b + (bottom.b - top.b) * t,
                     top.a + (bottom.a - top.a) * t);
    };

    // 中心扇形（凸多边形，折线按序展开）
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const Color center_col = color_at(cy);
    const size_t n = inner.size();
    for (size_t i = 0; i < n; ++i) {
        const RoundPt& a = inner[i];
        const RoundPt& b = inner[(i + 1) % n];
        push_vertex(cx, cy, center_col, 0, 0);
        push_vertex(a.x, a.y, color_at(a.y), 0, 0);
        push_vertex(b.x, b.y, color_at(b.y), 0, 0);
    }

    // 边缘渐隐带
    if (outer.size() == n) {
        for (size_t i = 0; i < n; ++i) {
            const RoundPt& a = inner[i];
            const RoundPt& b = inner[(i + 1) % n];
            const RoundPt& oa = outer[i];
            const RoundPt& ob = outer[(i + 1) % n];
            const Color ca = color_at(a.y);
            const Color cb = color_at(b.y);
            const Color ta(ca.r, ca.g, ca.b, 0.0f);
            const Color tb(cb.r, cb.g, cb.b, 0.0f);
            push_vertex(a.x, a.y, ca, 0, 0);
            push_vertex(b.x, b.y, cb, 0, 0);
            push_vertex(ob.x, ob.y, tb, 0, 0);

            push_vertex(a.x, a.y, ca, 0, 0);
            push_vertex(ob.x, ob.y, tb, 0, 0);
            push_vertex(oa.x, oa.y, ta, 0, 0);
        }
    }
}

void Renderer2D::draw_round_rect_border(float x, float y, float w, float h,
                                        const float radii[4], float thickness,
                                        const Color& color) {
    if (w <= 0.0f || h <= 0.0f || thickness <= 0.0f) return;
    if (thickness > (w < h ? w : h) * 0.5f) thickness = (w < h ? w : h) * 0.5f;

    constexpr float kAa = 0.5f;
    std::vector<RoundPt> edge;     // 外沿（描边外边界再向内 0.5，AA 的中点）
    std::vector<RoundPt> outer;    // 真正的外边界（alpha 0）
    std::vector<RoundPt> innr;     // 描边内边界
    const int seg = arc_segments(radii);
    build_round_outline(x, y, w, h, radii, -kAa, seg, edge);
    build_round_outline(x, y, w, h, radii, kAa, seg, outer);
    build_round_outline(x, y, w, h, radii, -kAa - thickness, seg, innr);
    if (edge.size() < 3 || innr.size() != edge.size() || outer.size() != edge.size()) return;

    const Color clear(color.r, color.g, color.b, 0.0f);
    const size_t n = edge.size();
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        // 外侧 AA 带
        push_vertex(edge[i].x, edge[i].y, color, 0, 0);
        push_vertex(outer[i].x, outer[i].y, clear, 0, 0);
        push_vertex(outer[j].x, outer[j].y, clear, 0, 0);

        push_vertex(edge[i].x, edge[i].y, color, 0, 0);
        push_vertex(outer[j].x, outer[j].y, clear, 0, 0);
        push_vertex(edge[j].x, edge[j].y, color, 0, 0);

        // 实心描边带
        push_vertex(edge[i].x, edge[i].y, color, 0, 0);
        push_vertex(edge[j].x, edge[j].y, color, 0, 0);
        push_vertex(innr[j].x, innr[j].y, color, 0, 0);

        push_vertex(edge[i].x, edge[i].y, color, 0, 0);
        push_vertex(innr[j].x, innr[j].y, color, 0, 0);
        push_vertex(innr[i].x, innr[i].y, color, 0, 0);
    }
}

void Renderer2D::draw_round_shadow(float x, float y, float w, float h,
                                   const float radii[4], float spread,
                                   const Color& color) {
    if (w <= 0.0f || h <= 0.0f || spread <= 0.0f || color.a <= 0.0f) return;

    // 把扩散半径切成同心环：alpha 按 (1 - d/spread)^2 衰减，看起来接近高斯，
    // 但只用顶点色——不需要额外的模糊 pass，也不会为了阴影引入离屏目标。
    constexpr int kRings = 8;
    const int seg = arc_segments(radii);
    std::vector<RoundPt> outer;
    std::vector<RoundPt> inner;
    for (int i = 0; i < kRings; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(kRings);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(kRings);
        const float d0 = spread * t0;
        const float d1 = spread * t1;
        const float tmid = (t0 + t1) * 0.5f;
        const float falloff = 1.0f - tmid;
        const Color c(color.r, color.g, color.b, color.a * falloff * falloff);

        build_round_outline(x, y, w, h, radii, d0, seg, inner);
        build_round_outline(x, y, w, h, radii, d1, seg, outer);
        if (inner.size() < 3 || inner.size() != outer.size()) continue;

        const size_t n = inner.size();
        for (size_t k = 0; k < n; ++k) {
            const size_t j = (k + 1) % n;
            push_vertex(inner[k].x, inner[k].y, c, 0, 0);
            push_vertex(outer[k].x, outer[k].y, c, 0, 0);
            push_vertex(outer[j].x, outer[j].y, c, 0, 0);

            push_vertex(inner[k].x, inner[k].y, c, 0, 0);
            push_vertex(outer[j].x, outer[j].y, c, 0, 0);
            push_vertex(inner[j].x, inner[j].y, c, 0, 0);
        }
    }
}


// 逐码点遍历 UTF-8（不做合法性校验：非法字节按单字节码点处理，字形查不到
// 自然跳过——比抛错/丢整串更接近"显示得出来的都显示"）。
template <typename F>
static void for_each_codepoint(const char* s, F&& fn) {
    if (!s) return;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        uint32_t cp = *p;
        int extra = 0;
        if (cp < 0x80u) {
            ++p;
        } else if ((cp & 0xE0u) == 0xC0u) {
            cp &= 0x1Fu; extra = 1;
        } else if ((cp & 0xF0u) == 0xE0u) {
            cp &= 0x0Fu; extra = 2;
        } else if ((cp & 0xF8u) == 0xF0u) {
            cp &= 0x07u; extra = 3;
        } else {
            ++p;                      // 续接字节/非法首字节：当作单字节跳过
            fn(static_cast<uint32_t>(0xFFFFFFFFu));
            continue;
        }
        if (extra > 0) {
            ++p;
            for (int i = 0; i < extra && (*p & 0xC0u) == 0x80u; ++i, ++p) {
                cp = (cp << 6) | (*p & 0x3Fu);
            }
        }
        fn(cp);
    }
}

void Renderer2D::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {
    if (!font_atlas_.texture() || using_fallback_font_) {
        // 回退图集的每个"字形"是一块实心色块（见 FontAtlas::create_fallback_atlas），
        // 逐字符画出来就是左上角一条灰色矩形 —— 看着像渲染出错，比不显示更糟。
        // 这里直接不画并告警一次：真正的信息在日志里，画面保持干净。
        if (!warned_no_font_) {
            warned_no_font_ = true;
            GLOG_WARN("Renderer2D: no usable font atlas, text overlay disabled "
                      "(ship res:/fonts/*.ttf or install system fonts)");
        }
        return;
    }

    const int   bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (raster <= 0.0f || font_size <= 0.0f) return;
    const float scale = font_size / raster;
    // 1:1 时把字形原点对齐到整像素：图集是按整数像素烘的，落在半像素上会被
    // 双线性采样平均成两列半灰，白白丢掉一半锐度（这正是"文字发虚"的典型成因）。
    // 缩放比明显不是 1 时不能对：那会把缩放的连续位置量化成台阶。
    const bool snap = std::fabs(scale - 1.0f) < 0.02f;
    float cursor_x = x;
    float cursor_y = y;

    for_each_codepoint(text.c_str(), [&](uint32_t cp) {
        if (cp == '\n') {
            cursor_x = x;
            cursor_y += raster * scale;
            return;
        }
        const Glyph* g = font_atlas_.get_glyph(bucket, cp);
        if (!g) return;

        float x0 = cursor_x + g->offset_x * scale;
        float y0 = cursor_y + g->offset_y * scale;
        if (snap) {
            x0 = std::floor(x0 + 0.5f);
            y0 = std::floor(y0 + 0.5f);
        }
        const float x1 = x0 + g->width * scale;
        const float y1 = y0 + g->height * scale;

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y0, color, g->uv1_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);
        push_text_vertex(x0, y1, color, g->uv0_x, g->uv1_y);

        cursor_x += g->advance * scale;
    });
}

float Renderer2D::measure_text(const char* text, float font_size) const {
    if (!text || !font_atlas_.texture() || using_fallback_font_) {
        return 0.0f;
    }
    const int   bucket = font_atlas_.bucket_for(font_size);
    const float raster = font_atlas_.bucket_size(bucket);
    if (raster <= 0.0f || font_size <= 0.0f) return 0.0f;
    const float scale = font_size / raster;
    float    best    = 0.0f;
    float    cursor  = 0.0f;
    // 多行取最宽一行（与 draw_text 的换行规则一致：'\n' 归零并下移）
    for_each_codepoint(text, [&](uint32_t cp) {
        if (cp == '\n') {
            if (cursor > best) best = cursor;
            cursor = 0.0f;
            return;
        }
        const Glyph* g = font_atlas_.get_glyph(bucket, cp);
        if (g) cursor += g->advance * scale;
    });
    return cursor > best ? cursor : best;
}

bool Renderer2D::ensure_text_raster_size(float px) {
    if (px < 6.0f) px = 6.0f;
    if (px > 128.0f) px = 128.0f;
    const float want = std::round(px);
    // init 之前调用只记"首烘尺寸"：那时图集还没建，正好一次按它烘好，
    // 不必"先烘 32 再重烘 13"（重烘 = 整张纹理重传）。
    text_raster_hint_ = want;
    if (!initialized_ || font_path_.empty() || using_fallback_font_) return false;
    // 已有档（12% 容差）就不再重烘：图集重建要重传整张纹理，不能每次问都做。
    if (font_atlas_.has_size(want)) return false;

    bool baked = false;
    auto bake = [this, want, &baked]() { baked = font_atlas_.ensure_size(ctx_, want); };
    if (ctx_ && ctx_->is_running()) {
        // 运行期重建纹理必须在渲染线程的上下文中做，而且必须**等它做完**：
        // 本帧随后的 draw_text 立刻会去查新档的字形表，异步会出现"表已换、
        // 纹理还没上"的中间态（这一帧的字要么丢要么采样到旧图）。
        ctx_->run_with_gpu_context_sync(bake);
    } else {
        bake();
    }
    if (!baked) return false;
    GLOG_INFO("Renderer2D: font atlas rebaked at {} px ({} size bucket(s))",
              static_cast<int>(want), font_atlas_.bucket_count());
    return true;
}

float Renderer2D::text_raster_size() const {
    return font_atlas_.font_size();
}

void Renderer2D::begin_overlay_pass(int screen_width, int screen_height) {    if (screen_width <= 0 || screen_height <= 0) return;

    // 场景 2D 的残留顶点先落地，免得被覆盖层的新相机重新变换一遍
    flush_batches();

    overlay_saved_.center   = camera_center_;
    overlay_saved_.zoom     = camera_zoom_;
    overlay_saved_.rotation = camera_rotation_;
    overlay_saved_.width    = screen_width_;
    overlay_saved_.height   = screen_height_;
    overlay_saved_.top_left = camera_top_left_origin_;
    overlay_saved_.active   = true;

    screen_width_  = static_cast<float>(screen_width);
    screen_height_ = static_cast<float>(screen_height);
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width_, screen_height_, 0.0f, -1.0f, 1.0f);
    set_camera(math::Vector2f(0.0f, 0.0f), 1.0f, true);

    if (context_alive()) {
        ctx_->set_viewport(0, 0, screen_width, screen_height);
        ctx_->set_depth_test(false);
        ctx_->set_blend(true);
        ctx_->set_cull_face(CullMode::None);
    }
    set_blend_mode(BlendMode::Alpha);
}

void Renderer2D::end_overlay_pass() {
    // 只提交顶点：不走 end_frame()，因为那里会跑阴影 pass 与 bloom 后处理
    //（覆盖层不需要、也不该被 bloom 洗一遍）。
    flush_batches();
    reset_scissor();

    if (!overlay_saved_.active) return;
    overlay_saved_.active = false;
    screen_width_  = overlay_saved_.width;
    screen_height_ = overlay_saved_.height;
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width_, screen_height_, 0.0f, -1.0f, 1.0f);
    set_camera(overlay_saved_.center, overlay_saved_.zoom, overlay_saved_.top_left,
               overlay_saved_.rotation);
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
        // 同 begin_frame 的重建路径：创建含直接 GL 调用，
        // 渲染线程运行中需先暂停（context 切回主线程）再创建。
        if (ctx_->is_running()) {
            ctx_->pause_render_thread();
            create_bloom_targets();
            ctx_->resume_render_thread();
        } else {
            create_bloom_targets();
        }
    }
}

void Renderer2D::draw_sprite(float x, float y, float w, float h,
                              RHITextureHandle texture, const Color& tint) {
    draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

void Renderer2D::draw_sprite_region(float x, float y, float w, float h,
                                     float u0, float v0, float u1, float v1,
                                     RHITextureHandle texture, const Color& tint) {
    // 纹理统一传 RHITextureHandle：无效句柄直接跳过，
    // 有效句柄在渲染线程执行时经 generation 校验解析，纹理已销毁则跳过本次绘制，
    // 彻底消除悬垂 ITexture* 风险。
    if (!texture.is_valid() || !context_alive() || !mesh_.is_valid() || !shader_.is_valid()) return;

    flush_batch(std::move(vertices_), false);

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    // 按值拷贝命令所需状态：view_proj_ 由主线程写，渲染线程只能读命令内的拷贝，
    // 否则构成跨线程数据竞争。句柄按值拷贝安全。
    const math::Matrix4f view_proj = view_proj_;
    const RHIMeshHandle mesh = mesh_;
    const RHIShaderHandle shader = shader_;
    ctx_->push_command([this, mesh, shader, view_proj, x0, y0, x1, y1, u0, v0, u1, v1, texture, tint](IRenderBackend* backend) {
        (void)backend;
        Vertex2D verts[6] = {
            {x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0},
            {x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0},
            {x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1},
            {x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0},
            {x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1},
            {x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1},
        };

        IMesh* mesh_ptr = ctx_->mesh(mesh);
        IShader* shader_ptr = ctx_->shader(shader);
        if (!mesh_ptr || !shader_ptr) return;

        // 句柄 → GPU 纹理：带 generation 校验，失效（已销毁/槽位复用）则跳过绘制
        ITexture* tex_ptr = ctx_->texture(texture);
        if (!tex_ptr) return;

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
        shader_ptr->set_mat4("uViewProj", view_proj);
        tex_ptr->bind(0);
        shader_ptr->set_int("uTexture", 0);
        shader_ptr->set_int("uUseTexture", 2);
        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                                      RHITextureHandle texture, const Color& tint) {
    if (!texture.is_valid() || !context_alive() || !mesh_.is_valid() || !shader_.is_valid()) return;

    flush_batch(std::move(vertices_), false);

    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);

    // 局部角点（顺时针）及其 UV，绕中心旋转后推入渲染命令。
    const math::Vector2f local[4] = {
        math::Vector2f(-hw, -hh),
        math::Vector2f( hw, -hh),
        math::Vector2f( hw,  hh),
        math::Vector2f(-hw,  hh)
    };
    const math::Vector2f uv[4] = {
        math::Vector2f(0.0f, 0.0f),
        math::Vector2f(1.0f, 0.0f),
        math::Vector2f(1.0f, 1.0f),
        math::Vector2f(0.0f, 1.0f)
    };
    math::Vector2f world[4];
    for (int i = 0; i < 4; ++i) {
        world[i] = math::Vector2f(
            cx + local[i].x * c - local[i].y * s,
            cy + local[i].x * s + local[i].y * c
        );
    }

    const math::Matrix4f view_proj = view_proj_;
    const RHIMeshHandle mesh = mesh_;
    const RHIShaderHandle shader = shader_;
    ctx_->push_command([this, mesh, shader, view_proj, world, uv, texture, tint](IRenderBackend* backend) {
        (void)backend;
        Vertex2D verts[6] = {
            {world[0].x, world[0].y, tint.r, tint.g, tint.b, tint.a, uv[0].x, uv[0].y},
            {world[1].x, world[1].y, tint.r, tint.g, tint.b, tint.a, uv[1].x, uv[1].y},
            {world[2].x, world[2].y, tint.r, tint.g, tint.b, tint.a, uv[2].x, uv[2].y},
            {world[0].x, world[0].y, tint.r, tint.g, tint.b, tint.a, uv[0].x, uv[0].y},
            {world[2].x, world[2].y, tint.r, tint.g, tint.b, tint.a, uv[2].x, uv[2].y},
            {world[3].x, world[3].y, tint.r, tint.g, tint.b, tint.a, uv[3].x, uv[3].y},
        };

        IMesh* mesh_ptr = ctx_->mesh(mesh);
        IShader* shader_ptr = ctx_->shader(shader);
        if (!mesh_ptr || !shader_ptr) return;

        ITexture* tex_ptr = ctx_->texture(texture);
        if (!tex_ptr) return;

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
        shader_ptr->set_mat4("uViewProj", view_proj);
        tex_ptr->bind(0);
        shader_ptr->set_int("uTexture", 0);
        shader_ptr->set_int("uUseTexture", 2);
        mesh_ptr->draw();
        shader_ptr->unbind();
    });
}

void Renderer2D::draw_lit_sprite(float x, float y, float w, float h,
                                  RHITextureHandle albedo, RHITextureHandle normal_map,
                                  const Color& tint) {
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, albedo, normal_map, tint,
                           0.0f, 0.0f, 1.0f, 1.0f);
}

void Renderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                         float u0, float v0, float u1, float v1,
                                         RHITextureHandle albedo, RHITextureHandle normal_map,
                                         const Color& tint,
                                         float nu0, float nv0,
                                         float nu1, float nv1) {
    if (!context_alive()) return;

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    // 批内只存句柄；albedo/normal_map 无效时解析会回退默认纹理（白图/平面法线），
    // 与旧裸指针 nullptr 行为一致
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

void Renderer2D::set_scissor(int x, int y, int w, int h) {
    // 裁剪是**状态**，绘制是**顶点**：vertices_ 里的几何要到 flush 才带着当时的
    // 状态下发，所以改裁剪前必须先把已缓冲的顶点提交掉，否则前后两段会一起
    // 吃到最后一次设置的裁剪域（内置 UI 的窗口裁剪就是这么用的）。
    flush_batches();
    if (ctx_) {
        ctx_->set_scissor(x, y, w, h);
    }
}

void Renderer2D::reset_scissor() {
    flush_batches();
    if (ctx_) {
        const auto size = screen_size();
        ctx_->set_scissor(0, 0, static_cast<int>(size.x), static_cast<int>(size.y));
    }
}

// ---------------------------------------------------------------------------
// 后端适配钩子的默认实现（= OpenGL 行为）。
// D3D12 等后端继承 Renderer2D 并覆盖这几个方法即可复用全部 2D 逻辑。
// ---------------------------------------------------------------------------
void Renderer2D::configure_depth_texture_for_sampling(ITexture* shadow_map) {
    auto* gl_shadow = static_cast<GLTexture*>(shadow_map);
    if (!gl_shadow || !gl_shadow->texture_id()) return;
    if (gl_dsa_available()) {
        glTextureParameteri(gl_shadow->texture_id(), GL_TEXTURE_COMPARE_MODE, GL_NONE);
    } else {
        glBindTexture(GL_TEXTURE_2D, gl_shadow->texture_id());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void Renderer2D::disable_color_writes_for_depth_only_target() {
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
}

uint32_t Renderer2D::create_default_1x1_texture(const unsigned char rgba[4]) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return static_cast<uint32_t>(id);
}

void Renderer2D::destroy_default_texture(uint32_t id) {
    if (id == 0) return;
    GLuint gl_id = static_cast<GLuint>(id);
    glDeleteTextures(1, &gl_id);
}

void Renderer2D::bind_default_texture_slot(int slot, uint32_t id) {
    glBindTextureUnit(static_cast<GLuint>(slot), static_cast<GLuint>(id));
}

} // namespace gryce_engine::render
