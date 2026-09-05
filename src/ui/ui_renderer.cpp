#include "ui_renderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#include <GL/glew.h>

#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/scroll_view.h"
#include "render/render_context.h"
#include "render/render_commands.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/opengl/gl_utils.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// UI 专用着色器源码
// ============================================================================

// 顶点着色器：支持位置、颜色、纹理坐标、模式、圆角 SDF 参数
static const char* k_ui_vert = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in float aMode;
layout(location = 4) in vec2 aRectOrigin;
layout(location = 5) in vec2 aRectSize;
layout(location = 6) in float aRadius;

out vec4 vColor;
out vec2 vTexCoord;
out float vMode;
out vec2 vRectOrigin;
out vec2 vRectSize;
out float vRadius;
out vec2 vPosition;

uniform mat4 uViewProj;

void main() {
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
    vMode = aMode;
    vRectOrigin = aRectOrigin;
    vRectSize = aRectSize;
    vRadius = aRadius;
    vPosition = aPos;
}
)";

// 片段着色器：支持纯色填充、字体图集、圆角矩形 SDF 三种模式
static const char* k_ui_frag = R"(
#version 330 core
in vec4 vColor;
in vec2 vTexCoord;
in float vMode;
in vec2 vRectOrigin;
in vec2 vRectSize;
in float vRadius;
in vec2 vPosition;

out vec4 FragColor;

uniform sampler2D uFontTexture;
uniform int uUseFontTexture;

// 圆角矩形 SDF（有符号距离场）
float rounded_rect_sdf(vec2 p, vec2 origin, vec2 size, float r) {
    vec2 rect_min = origin;
    vec2 rect_max = origin + size;
    vec2 q = abs(p - (rect_min + rect_max) * 0.5) - (rect_max - rect_min) * 0.5 + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    int mode = int(floor(vMode + 0.5));

    if (mode == 0) {
        // Mode 0: 纯色填充
        FragColor = vColor;
    } else if (mode == 1) {
        // Mode 1: 字体图集
        float alpha = texture(uFontTexture, vTexCoord).a;
        alpha = smoothstep(0.45, 0.55, alpha);
        if (alpha < 0.01) discard;
        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else if (mode == 2) {
        // Mode 2: 圆角矩形 SDF
        float d = rounded_rect_sdf(vPosition, vRectOrigin, vRectSize, vRadius);
        float alpha = 1.0 - smoothstep(0.0, 1.0, d);
        if (alpha < 0.01) discard;
        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else {
        FragColor = vColor;
    }
}
)";

// ============================================================================
// 字体辅助
// ============================================================================

namespace {

// 获取系统字体目录
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
#endif
    return "C:\\Windows\\Fonts\\";
}

// UTF-8 字符数（简化版，仅用于 ASCII 文本宽度估算）
int utf8_length(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] & 0xC0) != 0x80) ++count;
    }
    return count;
}

} // namespace

// ============================================================================
// UIRenderer 实现
// ============================================================================

UIRenderer::UIRenderer() = default;

UIRenderer::~UIRenderer() {
    shutdown();
}

bool UIRenderer::init(gryce_engine::render::RenderContext* ctx) {
    if (initialized_ || !ctx) return false;
    ctx_ = ctx;

    // 1. 编译 UI 着色器
    shader_ = ctx_->create_shader();
    auto* shader_ptr = ctx_->shader(shader_);
    if (!shader_.is_valid() || !shader_ptr ||
        !shader_ptr->compile(k_ui_vert, k_ui_frag)) {
        GLOG_ERROR("UIRenderer: failed to compile UI shader");
        if (shader_.is_valid()) {
            ctx_->destroy_shader(shader_);
            shader_ = gryce_engine::render::RHIShaderHandle{};
        }
        return false;
    }
    GLOG_INFO("UIRenderer: UI shader compiled");

    // 2. 创建 mesh
    mesh_ = ctx_->create_mesh();
    if (!mesh_.is_valid()) {
        GLOG_ERROR("UIRenderer: failed to create mesh");
        ctx_->destroy_shader(shader_);
        shader_ = gryce_engine::render::RHIShaderHandle{};
        return false;
    }

    // 3. 创建字体图集
    font_atlas_ = new gryce_engine::render::FontAtlas();

    // 优先使用项目内置 TTF
    {
        std::string bundled = gryce_engine::resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf");
        if (bundled.empty() || !std::filesystem::exists(bundled)) {
            bundled = gryce_engine::resources::Project::instance().root() + "/third_party/imgui/misc/fonts/Roboto-Medium.ttf";
        }
        if (std::filesystem::exists(bundled)) {
            if (font_atlas_->init(ctx_, bundled, 32.0f)) {
                GLOG_INFO("UIRenderer: loaded bundled font '{}'", bundled);
            }
        }
    }

    // 内置字体失败时尝试系统字体
    if (!font_atlas_->texture()) {
        std::string font_dir = get_system_font_dir();
        const char* font_names[] = {
            "arial.ttf",
            "segoeui.ttf",
            "tahoma.ttf",
            "calibri.ttf",
            "msyh.ttc",
            "simhei.ttf",
            nullptr
        };
        for (int i = 0; font_names[i]; ++i) {
            std::string font_path = font_dir + font_names[i];
            if (font_atlas_->init(ctx_, font_path, 32.0f)) {
                GLOG_INFO("UIRenderer: loaded system font '{}'", font_path);
                break;
            }
        }
    }

    if (!font_atlas_->texture()) {
        GLOG_WARN("UIRenderer: no font loaded, creating fallback atlas");
        font_atlas_->create_fallback_atlas(ctx_, 32.0f);
    }

    // 4. 预分配顶点/索引缓冲区
    vertices_.reserve(8192);
    indices_.reserve(16384);

    initialized_ = true;
    GLOG_INFO("UIRenderer initialized");
    return true;
}

void UIRenderer::shutdown() {
    if (!initialized_) return;

    if (ctx_) {
        // 等待渲染线程完成，确保命令 lambda 不再访问 this
        if (ctx_->is_running()) {
            ctx_->present();
            ctx_->wait_for_idle();
        }

        if (font_atlas_) {
            font_atlas_->destroy(ctx_);
            delete font_atlas_;
            font_atlas_ = nullptr;
        }
        if (mesh_.is_valid()) {
            ctx_->destroy_mesh(mesh_);
            mesh_ = gryce_engine::render::RHIMeshHandle{};
        }
        if (shader_.is_valid()) {
            ctx_->destroy_shader(shader_);
            shader_ = gryce_engine::render::RHIShaderHandle{};
        }
    }

    vertices_.clear();
    indices_.clear();
    initialized_ = false;
    ctx_ = nullptr;
    GLOG_INFO("UIRenderer shutdown");
}

void UIRenderer::begin_frame(float screen_w, float screen_h) {
    if (!initialized_ || !ctx_) return;

    screen_w_ = screen_w;
    screen_h_ = screen_h;

    // 清空上一帧的顶点数据
    vertices_.clear();
    indices_.clear();
    in_frame_ = true;
    scissor_active_ = false;

    // 设置正交投影：屏幕左上角为原点，Y 向下
    ortho_ = math::Matrix4f::ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);

    // 设置 OpenGL 状态：关闭深度测试、开启混合（Alpha 混合）、无背面剔除
    ctx_->set_viewport(0, 0, static_cast<int>(screen_w), static_cast<int>(screen_h));
    ctx_->set_depth_test(false);
    ctx_->set_blend(true);
    ctx_->set_cull_face(gryce_engine::render::CullMode::None);
}

void UIRenderer::end_frame() {
    if (!in_frame_ || !initialized_ || !ctx_) return;

    // 提交所有顶点
    flush();

    // 恢复深度测试
    ctx_->set_depth_test(true);
    ctx_->set_blend(false);

    // 重置裁剪
    if (scissor_active_) {
        ctx_->set_scissor(0, 0, static_cast<int>(screen_w_), static_cast<int>(screen_h_));
        scissor_active_ = false;
    }

    in_frame_ = false;
}

void UIRenderer::flush() {
    if (vertices_.empty() || !ctx_ || !mesh_.is_valid() || !shader_.is_valid()) {
        return;
    }

    // 确保顶点和索引数量匹配
    if (indices_.empty()) {
        // 如果没有索引，每 3 个顶点构成一个三角形
        // 但我们的顶点是用 quad 方式生成的，所以必须有索引
        // 如果索引为空，使用非索引绘制
    }

    // 按值拷贝所有数据到 shared_ptr，避免与主线程的写竞争
    auto verts_shared = std::make_shared<std::vector<UIVertex>>(std::move(vertices_));
    auto idx_shared = std::make_shared<std::vector<uint32_t>>(std::move(indices_));
    const math::Matrix4f ortho = ortho_;
    const gryce_engine::render::RHIShaderHandle shader = shader_;
    const gryce_engine::render::RHIMeshHandle mesh = mesh_;
    const bool has_font = font_atlas_ && font_atlas_->texture();
    const gryce_engine::render::RHITextureHandle font_tex = has_font ? font_atlas_->texture_handle() : gryce_engine::render::RHITextureHandle{};

    ctx_->push_command([verts_shared, idx_shared, ortho, shader, mesh, has_font, font_tex](gryce_engine::render::IRenderBackend* backend) {
        auto* mesh_ptr = backend->mesh(mesh);
        auto* shader_ptr = backend->shader(shader);
        if (!mesh_ptr || !shader_ptr) return;

        // 上传顶点数据
        mesh_ptr->upload_vertices(
            verts_shared->data(),
            static_cast<uint32_t>(verts_shared->size() * sizeof(UIVertex)),
            static_cast<uint32_t>(verts_shared->size()));

        // 设置顶点布局
        gryce_engine::render::VertexLayout layout;
        layout.stride = sizeof(UIVertex);
        layout.attributes = {
            {0, gryce_engine::render::VertexType::Float2, false, 0},                                    // aPos
            {1, gryce_engine::render::VertexType::Float4, false, 2 * sizeof(float)},                     // aColor
            {2, gryce_engine::render::VertexType::Float2, false, 6 * sizeof(float)},                     // aTexCoord
            {3, gryce_engine::render::VertexType::Float,  false, 8 * sizeof(float)},                     // aMode
            {4, gryce_engine::render::VertexType::Float2, false, 9 * sizeof(float)},                     // aRectOrigin
            {5, gryce_engine::render::VertexType::Float2, false, 11 * sizeof(float)},                    // aRectSize
            {6, gryce_engine::render::VertexType::Float,  false, 13 * sizeof(float)}                     // aRadius
        };
        mesh_ptr->set_layout(layout);

        // 绑定着色器
        shader_ptr->bind();
        shader_ptr->set_mat4("uViewProj", ortho);

        // 绑定字体纹理
        if (has_font && font_tex.is_valid()) {
            auto* font_tex_ptr = backend->texture(font_tex);
            if (font_tex_ptr) {
                font_tex_ptr->bind(0);
            }
            shader_ptr->set_int("uFontTexture", 0);
            shader_ptr->set_int("uUseFontTexture", 1);
        } else {
            shader_ptr->set_int("uUseFontTexture", 0);
        }

        // 如果有索引，使用索引绘制
        if (!idx_shared->empty()) {
            mesh_ptr->upload_indices(
                idx_shared->data(),
                static_cast<uint32_t>(idx_shared->size() * sizeof(uint32_t)),
                static_cast<uint32_t>(idx_shared->size()));
            mesh_ptr->draw_indexed();
        } else {
            // 无索引时使用非索引绘制（每 3 个顶点一个三角形）
            mesh_ptr->draw();
        }

        shader_ptr->unbind();
    });

    // 清空已移走的容器（move 后容器为空，但保证有效）
    vertices_.clear();
    indices_.clear();
}

void UIRenderer::ensure_capacity(size_t extra_verts, size_t extra_indices) {
    if (vertices_.capacity() < vertices_.size() + extra_verts) {
        vertices_.reserve(std::max(vertices_.capacity() * 2, vertices_.size() + extra_verts));
    }
    if (indices_.capacity() < indices_.size() + extra_indices) {
        indices_.reserve(std::max(indices_.capacity() * 2, indices_.size() + extra_indices));
    }
}

void UIRenderer::push_quad(float x, float y, float w, float h,
                           float u0, float v0, float u1, float v1,
                           const Color& color, float mode,
                           float rx, float ry, float rw, float rh, float radius) {
    const float x0 = x, y0 = y;
    const float x1 = x + w, y1 = y + h;

    ensure_capacity(4, 6);

    const uint32_t base = static_cast<uint32_t>(vertices_.size());

    // 4 个顶点
    // 左上
    vertices_.push_back({x0, y0, color.r, color.g, color.b, color.a, u0, v0, mode, rx, ry, rw, rh, radius});
    // 右上
    vertices_.push_back({x1, y0, color.r, color.g, color.b, color.a, u1, v0, mode, rx, ry, rw, rh, radius});
    // 右下
    vertices_.push_back({x1, y1, color.r, color.g, color.b, color.a, u1, v1, mode, rx, ry, rw, rh, radius});
    // 左下
    vertices_.push_back({x0, y1, color.r, color.g, color.b, color.a, u0, v1, mode, rx, ry, rw, rh, radius});

    // 6 个索引（两个三角形构成一个 quad）
    indices_.push_back(base);
    indices_.push_back(base + 1);
    indices_.push_back(base + 2);
    indices_.push_back(base);
    indices_.push_back(base + 2);
    indices_.push_back(base + 3);
}

void UIRenderer::draw_rect(float x, float y, float w, float h, const Color& color) {
    if (w <= 0.0f || h <= 0.0f) return;
    // 纯色填充：mode=0，不使用 SDF
    push_quad(x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, color, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

void UIRenderer::draw_rounded_rect(float x, float y, float w, float h,
                                   float radius, const Color& color) {
    if (w <= 0.0f || h <= 0.0f) return;
    radius = std::min(radius, std::min(w, h) * 0.5f);
    if (radius <= 0.5f) {
        // 半径太小，使用纯色填充
        draw_rect(x, y, w, h, color);
        return;
    }

    // 使用圆角矩形 SDF：mode=2，传递矩形位置和圆角半径
    // 片段位置在 vPosition 中插值，SDF 计算相对于矩形原点 (x, y)
    // 使用纹理坐标传递片段位置（实际在 shader 中用 vPosition）
    // 这里将矩形参数传入 SDF 参数
    push_quad(x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, color, 2.0f,
              x, y, w, h, radius);
}

void UIRenderer::draw_text(float x, float y, const std::string& text,
                           float font_size, const Color& color) {
    if (text.empty() || !font_atlas_) return;

    // 缩放因子：字体图集是 32px，用户可能指定不同字号
    const float scale = font_size / font_atlas_->font_size();

    float cursor_x = x;
    const float cursor_y = y;

    for (size_t i = 0; i < text.size();) {
        // 处理 UTF-8 编码
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t codepoint;
        if (c < 0x80) {
            codepoint = c;
            ++i;
        } else if (c < 0xE0) {
            codepoint = (c & 0x1F) << 6 | (text[i + 1] & 0x3F);
            i += 2;
        } else if (c < 0xF0) {
            codepoint = (c & 0x0F) << 12 | (text[i + 1] & 0x3F) << 6 | (text[i + 2] & 0x3F);
            i += 3;
        } else {
            codepoint = (c & 0x07) << 18 | (text[i + 1] & 0x3F) << 12 | (text[i + 2] & 0x3F) << 6 | (text[i + 3] & 0x3F);
            i += 4;
        }

        const auto* glyph = font_atlas_->get_glyph(codepoint);
        if (!glyph) {
            // 无字形时跳过
            cursor_x += 8.0f * scale; // 近似空格宽度
            continue;
        }

        const float gx = cursor_x + glyph->offset_x * scale;
        const float gy = cursor_y + glyph->offset_y * scale;
        const float gw = glyph->width * scale;
        const float gh = glyph->height * scale;

        if (gw > 0.0f && gh > 0.0f) {
            // 字体图集模式：mode=1，使用 UV 坐标采样字体纹理
            push_quad(gx, gy, gw, gh,
                      glyph->uv0_x, glyph->uv0_y, glyph->uv1_x, glyph->uv1_y,
                      color, 1.0f,
                      0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        }

        cursor_x += glyph->advance * scale;
    }
}

void UIRenderer::draw_focus_ring(float x, float y, float w, float h,
                                 float radius, const Color& color) {
    // 焦点环：外扩 2px 的圆角矩形轮廓
    // 使用 SDF 模式绘制，但只绘制边缘
    const float inset = 2.0f;
    const float ox = x - inset;
    const float oy = y - inset;
    const float ow = w + inset * 2.0f;
    const float oh = h + inset * 2.0f;
    const float orad = radius + inset;

    // 目前简单实现：绘制外圈圆角矩形和内圈透明矩形
    // 先绘制外圈圆角矩形
    draw_rounded_rect(ox, oy, ow, oh, orad, color);
    // 再绘制内圈矩形（用背景色覆盖中间部分，实现轮廓效果）
    // 注意：这里用半透明覆盖来实现轮廓效果不够精确
    // 更精确的方式需要 SDF 轮廓，但当前渲染器暂不支持
    // 使用一个较薄的圆角矩形作为替代
    const float bw = 2.0f;
    draw_rounded_rect(x + bw, y + bw, w - bw * 2.0f, h - bw * 2.0f,
                      std::max(0.0f, radius - bw),
                      Color(color.r, color.g, color.b, 0.0f));
}

void UIRenderer::set_scissor(int x, int y, int w, int h) {
    scissor_x_ = x;
    scissor_y_ = y;
    scissor_w_ = w;
    scissor_h_ = h;
    scissor_active_ = true;

    // 先刷新当前批次，确保之前绘制的顶点已经提交
    flush();

    if (ctx_) {
        ctx_->set_scissor(x, y, w, h);
    }
}

void UIRenderer::reset_scissor() {
    scissor_active_ = false;

    // 先刷新当前批次
    flush();

    if (ctx_) {
        // 全屏 scissor（实际关闭裁剪）
        ctx_->set_scissor(0, 0, static_cast<int>(screen_w_), static_cast<int>(screen_h_));
    }
}

void UIRenderer::generate_mesh(Widget* root, const std::vector<Widget*>& modals) {
    if (!in_frame_ || !root) return;

    // 先设置根控件大小为全屏
    root->set_bounds(Rect{0.0f, 0.0f, screen_w_, screen_h_});
    root->mark_layout_dirty();

    // 遍历根控件树
    draw_widget_mesh(root);

    // 遍历模态控件
    for (auto* modal : modals) {
        if (!modal) continue;
        modal->set_bounds(Rect{0.0f, 0.0f, screen_w_, screen_h_});
        modal->mark_layout_dirty();
        draw_widget_mesh(modal);
    }
}

void UIRenderer::draw_widget_mesh(Widget* widget) {
    if (!widget || !widget->visible()) return;

    // 面板布局更新
    if (auto* panel = dynamic_cast<Panel*>(widget)) {
        panel->relayout();
    }

    // ScrollView 裁剪：子控件完全在视口外时跳过
    if (widget->parent() && std::strcmp(widget->parent()->type_name(), "ScrollView") == 0) {
        const auto& vp = widget->parent()->bounds();
        const auto& b = widget->bounds();
        if (b.x + b.w < vp.x || b.x > vp.x + vp.w ||
            b.y + b.h < vp.y || b.y > vp.y + vp.h) {
            return;
        }
    }

    // 进入 ScrollView 时设置裁剪
    bool is_scroll = dynamic_cast<ScrollView*>(widget) != nullptr;
    if (is_scroll) {
        const auto& b = widget->bounds();
        set_scissor(static_cast<int>(b.x), static_cast<int>(b.y),
                    static_cast<int>(b.w + 0.5f), static_cast<int>(b.h + 0.5f));
    }

    // 绘制控件本身
    {
        const auto& bounds = widget->bounds();
        if (bounds.w > 0.0f && bounds.h > 0.0f) {
            const Style& s = widget->style();
            float alpha = widget->opacity() * s.opacity;
            if (!widget->enabled()) alpha *= 0.4f;

            if (alpha > 0.0f) {
                Color bg = s.background;
                if (widget->pressed()) bg = s.background_pressed;
                else if (widget->hovered()) bg = s.background_hover;

                // 有背景色或设置了背景标记
                if (s.has_background || s.background.a > 0.0f ||
                    bg.r > 0.0f || bg.g > 0.0f || bg.b > 0.0f) {
                    if (s.border_width > 0.0f) {
                        // 先画外圈圆角矩形（边框色）
                        draw_rounded_rect(bounds.x, bounds.y, bounds.w, bounds.h,
                                          s.border_radius,
                                          Color(s.border_color.r, s.border_color.g, s.border_color.b, s.border_color.a * alpha));
                        // 再画内缩背景
                        const float bw = s.border_width;
                        draw_rounded_rect(bounds.x + bw, bounds.y + bw,
                                          bounds.w - bw * 2.0f, bounds.h - bw * 2.0f,
                                          std::max(0.0f, s.border_radius - bw),
                                          Color(bg.r, bg.g, bg.b, bg.a * alpha));
                    } else {
                        draw_rounded_rect(bounds.x, bounds.y, bounds.w, bounds.h,
                                          s.border_radius,
                                          Color(bg.r, bg.g, bg.b, bg.a * alpha));
                    }
                }

                // 焦点环
                if (widget->focused() && widget->enabled()) {
                    draw_focus_ring(bounds.x, bounds.y, bounds.w, bounds.h,
                                    s.border_radius,
                                    Color(0.4f, 0.6f, 1.0f, 0.8f * alpha));
                }
            }
        }
    }

    // 绘制子控件
    for (auto* child : widget->children()) {
        draw_widget_mesh(child);
    }

    // 离开 ScrollView 时恢复裁剪
    if (is_scroll) {
        reset_scissor();
    }
}

float UIRenderer::text_width(const std::string& s, float font_size) const {
    if (!font_atlas_) return 0.0f;
    const float scale = font_size / font_atlas_->font_size();
    float width = 0.0f;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t codepoint;
        if (c < 0x80) {
            codepoint = c; ++i;
        } else if (c < 0xE0) {
            codepoint = (c & 0x1F) << 6 | (s[i + 1] & 0x3F); i += 2;
        } else if (c < 0xF0) {
            codepoint = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F); i += 3;
        } else {
            codepoint = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F); i += 4;
        }
        const auto* glyph = font_atlas_->get_glyph(codepoint);
        if (glyph) {
            width += glyph->advance * scale;
        } else {
            width += 8.0f * scale;
        }
    }
    return width;
}

} // namespace GryceEngineUtils::ui