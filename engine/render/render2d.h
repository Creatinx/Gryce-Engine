#pragma once

#include <string>
#include <vector>

#include "render/export.h"
#include "math/math.h"
#include "render/rhi_handle.h"

namespace gryce_engine {
namespace assets { struct TextureData; }
namespace render {

// ---------------------------------------------------------------------------
// Color — RGBA 颜色
// ---------------------------------------------------------------------------
struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f)
        : r(r_), g(g_), b(b_), a(a_) {}

    static constexpr Color white()   { return Color(1.0f, 1.0f, 1.0f, 1.0f); }
    static constexpr Color black()   { return Color(0.0f, 0.0f, 0.0f, 1.0f); }
    static constexpr Color red()     { return Color(1.0f, 0.0f, 0.0f, 1.0f); }
    static constexpr Color green()   { return Color(0.0f, 1.0f, 0.0f, 1.0f); }
    static constexpr Color blue()    { return Color(0.0f, 0.0f, 1.0f, 1.0f); }
    static constexpr Color yellow()  { return Color(1.0f, 1.0f, 0.0f, 1.0f); }
    static constexpr Color cyan()    { return Color(0.0f, 1.0f, 1.0f, 1.0f); }
    static constexpr Color magenta() { return Color(1.0f, 0.0f, 1.0f, 1.0f); }
    static constexpr Color gray(float v) { return Color(v, v, v, 1.0f); }
    static constexpr Color orange()  { return Color(1.0f, 0.65f, 0.0f, 1.0f); }

    bool operator==(const Color& other) const = default;
};

class RenderContext;
class IShader;
class IMesh;
class ITexture;

// ---------------------------------------------------------------------------
// LightType2D — 2D 光源类型
// ---------------------------------------------------------------------------
enum class LightType2D {
    Point,
    Directional,
    Spot
};

// ---------------------------------------------------------------------------
// Light2D — 统一 2D 光源描述
// ---------------------------------------------------------------------------
struct Light2D {
    LightType2D type = LightType2D::Point;
    math::Vector2f position;     // Point / Spot 的世界位置
    math::Vector2f direction;    // Directional / Spot 的方向（应归一化）
    Color color = Color::white();
    float intensity = 1.0f;
    float radius = 200.0f;       // Point / Spot 的影响半径
    float range = 1000.0f;       // Directional / Spot 的有效范围
    float spot_angle = 45.0f;    // Spot 外锥角（度）
    float spot_softness = 0.2f;  // Spot 内外锥过渡（0~1，0 为硬边）
};

// ---------------------------------------------------------------------------
// BloomParams — 2D Bloom 后处理参数
// ---------------------------------------------------------------------------
struct BloomParams {
    bool enabled = false;
    float threshold = 1.0f;
    float intensity = 0.5f;
    int blur_passes = 2;         // 可分离高斯模糊 pass 数
};

// ---------------------------------------------------------------------------
// BlendMode — 2D 渲染混合模式
// ---------------------------------------------------------------------------
enum class BlendMode {
    Opaque,   // 不混合
    Alpha,    // SrcAlpha / OneMinusSrcAlpha
    Additive  // SrcAlpha / One
};

// ---------------------------------------------------------------------------
// IRenderer2D — 2D 图形渲染接口
// 支持：矩形、n 边形、圆形、文字
// 所有坐标系以屏幕左上角为原点，向右为 +X，向下为 +Y
// 注意：以 std::unique_ptr<IRenderer2D> 跨 DLL 边界返回，必须导出。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API IRenderer2D {
public:
    virtual ~IRenderer2D() = default;

    // 初始化（必须在 RenderContext::start() 之前调用）
    virtual void init(RenderContext* ctx) = 0;
    virtual void shutdown() = 0;

    // 每帧开始：设置 ortho 投影、blend/depth 状态
    virtual void begin_frame(float screen_width, float screen_height) = 0;
    // 每帧结束：提交所有顶点并绘制
    virtual void end_frame() = 0;

    // 设置当前混合模式（默认 Alpha；影响后续 2D 绘制）
    virtual void set_blend_mode(BlendMode mode) { (void)mode; }

    // -----------------------------------------------------------------------
    // 2D 摄像机（可选）
    // -----------------------------------------------------------------------
    // 设置当前活动摄像机的世界中心坐标、缩放与 Z 旋转（弧度）。
    // 默认不调用时等价于 center=(0,0), zoom=1, rotation=0。
    // top_left_origin=true 时坐标系以屏幕左上角为原点（UI/HUD 层），
    // 此时 center=(0,0), zoom=1 表示世界 (0,0) 对齐屏幕左上角。
    virtual void set_camera(const math::Vector2f& center, float zoom, bool top_left_origin = false,
                            float rotation = 0.0f) {
        (void)center; (void)zoom; (void)top_left_origin; (void)rotation;
    }

    // 获取当前摄像机状态（用于 RenderSystem2D 做世界→屏幕变换）
    virtual math::Vector2f camera_center() const { return math::Vector2f::zero(); }
    virtual float camera_zoom() const { return 1.0f; }
    virtual float camera_rotation() const { return 0.0f; }

    // 将世界坐标转换为屏幕像素坐标（考虑当前 camera center/zoom + viewport）
    virtual math::Vector2f world_to_screen(const math::Vector2f& world) const { return world; }

    // 当前帧视口尺寸（像素）
    virtual math::Vector2f screen_size() const { return math::Vector2f::zero(); }

    // 绘制四边形（矩形填充）
    virtual void draw_rect(float x, float y, float w, float h, const Color& color) = 0;

    // 绘制 n 边形（凸多边形，使用 triangle fan）
    virtual void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) = 0;

    // 绘制圆形（填充）
    virtual void draw_circle(float cx, float cy, float r, int segments, const Color& color) = 0;

    // -----------------------------------------------------------------------
    // 圆角矩形族（UI 美化：圆角、渐变、描边、外阴影）
    // -----------------------------------------------------------------------
    // radii 按「左上、右上、右下、左下」排列，单位像素，会被钳到 min(w,h)/2。
    // 默认实现退化成直角矩形：不支持的后端（或第三方 IRenderer2D 实现）不会
    // 因为 UI 引用了圆角而编不过或画不出东西——最差只是看着方正。
    //
    // 为什么不用 SDF + 自定义 shader：2D 的顶点格式只有 position/color/uv，
    // 塞不进"矩形半宽/圆角半径"这类逐图元参数；而在 CPU 侧把圆角展开成折线
    // 扇形（边缘再加一条 1 像素的 alpha 渐隐带做抗锯齿）用的是现成的批次与
    // 混合状态，三后端零改动，也不会为 UI 多编译一份着色器变体。
    virtual void draw_round_rect(float x, float y, float w, float h,
                                 const float radii[4], const Color& color) {
        (void)radii;
        draw_rect(x, y, w, h, color);
    }
    // 竖直渐变填充（top → bottom），圆角同上。
    virtual void draw_round_rect_gradient(float x, float y, float w, float h,
                                          const float radii[4],
                                          const Color& top, const Color& bottom) {
        (void)radii; (void)bottom;
        draw_rect(x, y, w, h, top);
    }
    // 圆角描边，线宽向内生长（外轮廓尺寸不变）。
    virtual void draw_round_rect_border(float x, float y, float w, float h,
                                        const float radii[4], float thickness,
                                        const Color& color) {
        (void)radii;
        if (thickness <= 0.0f || w <= 0.0f || h <= 0.0f) return;
        draw_rect(x, y, w, thickness, color);
        draw_rect(x, y + h - thickness, w, thickness, color);
        draw_rect(x, y, thickness, h, color);
        draw_rect(x + w - thickness, y, thickness, h, color);
    }
    // 圆角外阴影：spread 为扩散半径，color 的 alpha 是贴近矩形处的浓度。
    virtual void draw_round_shadow(float x, float y, float w, float h,
                                   const float radii[4], float spread,
                                   const Color& color) {
        (void)x; (void)y; (void)w; (void)h; (void)radii; (void)spread; (void)color;
    }

    // 绘制文字（UTF-8 字符串，按码点查字形；图集里没有的码点跳过）。
    // y 是**基线**，不是行盒顶。
    virtual void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) = 0;

    // 文本宽度（同一套字形推进量，含 UTF-8 解码）。返回 0 表示本渲染器
    // 不提供度量（无字体图集/后端未实现），调用方应退回估算宽度。
    // 存在的意义：自动尺寸控件（按钮宽 = 文字宽 + 内边距）与文字居中都
    // 必须和真正画出来的宽度用同一个口径，否则宽度会漂。
    virtual float measure_text(const char* text, float font_size) const {
        (void)text; (void)font_size; return 0.0f;
    }

    // -----------------------------------------------------------------------
    // 字形栅格尺寸（DPI / 缩放）
    // -----------------------------------------------------------------------
    // 图集里的字形是**按某个像素尺寸烘出来**的位图：拿 13px 的图去画 26px
    // 就是 2 倍放大（糊），拿 32px 的图去画 13px 是缩一半（发虚、笔画变淡）。
    // 想要 1:1 清晰就只有一个办法——让烘图尺寸跟着**实际绘制尺寸**走。
    //
    // 调用约定：宿主（内置 UI 轨）传入它将要用的物理字号，返回 true 表示
    // 图集确实被重烘了。实现应带滞回（差得太少就别重建，重传纹理有代价）。
    // 默认空实现：不支持的后端保持原样，调用方退回"按原尺寸缩放绘制"。
    virtual bool ensure_text_raster_size(float px) { (void)px; return false; }

    // 当前图集的栅格字号（0 = 后端没有字体图集）。绘制方据此判断能否 1:1。
    virtual float text_raster_size() const { return 0.0f; }

    // -----------------------------------------------------------------------
    // UI 覆盖层 pass
    // -----------------------------------------------------------------------
    // 内置 UI 的绘制入口：与场景 2D 共用同一个 Renderer2D，但**独立成帧**——
    // 保存并切换相机到"左上角原点、1:1"、关掉 bloom/阴影、清空批次，结束时
    // 直接提交顶点并恢复相机。这样 UI 命令既不需要插进场景 2D 的批次里
    //（那要求 UI 在场景 pass 之前构建），也不会被 2D bloom 洗一遍。
    // 默认空实现：不支持的后端退化成"UI 不显示"，不影响场景渲染。
    virtual void begin_overlay_pass(int screen_width, int screen_height) {
        (void)screen_width; (void)screen_height;
    }
    virtual void end_overlay_pass() {}

    // -----------------------------------------------------------------------
    // 2D 光照接口
    // -----------------------------------------------------------------------
    // 设置环境光（默认黑色，即无环境光）
    virtual void set_ambient_light(const Color& color) { (void)color; }

    // 添加统一 2D 光源（Point / Directional / Spot）
    virtual void add_light(const Light2D& light) { (void)light; }

    // 添加一个点光源；保留旧接口，内部转发到 add_light
    virtual void add_point_light(const math::Vector2f& pos, float radius,
                                  const Color& color, float intensity);

    // 清空已收集的光源（每帧 begin_frame 内部自动调用）
    virtual void reset_lights() {}

    // 绘制不受光照的贴图精灵
    // 纹理统一使用 RHITextureHandle：命令在渲染线程执行时经 generation 校验
    // 解析，句柄失效（纹理已销毁/槽位复用）则安全跳过，杜绝悬垂 ITexture*。
    virtual void draw_sprite(float x, float y, float w, float h,
                              RHITextureHandle texture, const Color& tint = Color::white()) {
        draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
    }

    // 绘制不受光照的贴图精灵区域（UV 裁剪）
    virtual void draw_sprite_region(float x, float y, float w, float h,
                                     float u0, float v0, float u1, float v1,
                                     RHITextureHandle texture, const Color& tint = Color::white()) {
        (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1; (void)v1;
        (void)texture; (void)tint;
    }

    // 绘制绕中心旋转的不受光照贴图精灵
    virtual void draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                                      RHITextureHandle texture, const Color& tint = Color::white()) {
        (void)x; (void)y; (void)w; (void)h; (void)rotation; (void)texture; (void)tint;
    }

    // 绘制受光照的贴图精灵；normal_map 句柄无效时使用默认平面法线
    virtual void draw_lit_sprite(float x, float y, float w, float h,
                                  RHITextureHandle albedo, RHITextureHandle normal_map,
                                  const Color& tint = Color::white()) {
        draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
                               albedo, normal_map, tint,
                               0.0f, 0.0f, 1.0f, 1.0f);
    }

    // 绘制受光照的贴图精灵区域（UV 裁剪）
    virtual void draw_lit_sprite_region(float x, float y, float w, float h,
                                         float u0, float v0, float u1, float v1,
                                         RHITextureHandle albedo, RHITextureHandle normal_map,
                                         const Color& tint = Color::white(),
                                         float nu0 = 0.0f, float nv0 = 0.0f,
                                         float nu1 = 1.0f, float nv1 = 1.0f) {
        (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1; (void)v1;
        (void)albedo; (void)normal_map; (void)tint;
        (void)nu0; (void)nv0; (void)nu1; (void)nv1;
    }

    // 绘制阴影遮挡物（用于 2D 硬阴影）
    virtual void draw_shadow_caster(float x, float y, float w, float h) {
        (void)x; (void)y; (void)w; (void)h;
    }

    // 设置裁剪矩形（scissor test），屏幕坐标
    virtual void set_scissor(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
    // 恢复全屏裁剪
    virtual void reset_scissor() {}

    // 设置 Bloom 后处理参数
    virtual void set_bloom(const BloomParams& params) { (void)params; }

    // 从 CPU 侧 TextureData 创建并上传 GPU 纹理（异步到渲染线程）
    virtual RHITextureHandle create_texture_from_data(const assets::TextureData* data) {
        (void)data;
        return RHITextureHandle{};
    }

    // 将 RHI 纹理句柄解析为实际 GPU 纹理指针。
    // 仅供主线程查询纹理尺寸等元数据使用；绘制路径一律传 handle，
    // 不要把返回的裸指针捕获进渲染命令（有悬垂风险）。
    virtual ITexture* resolve_texture(RHITextureHandle handle) const {
        (void)handle;
        return nullptr;
    }
};

} // namespace render
} // namespace gryce_engine
