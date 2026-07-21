#pragma once

#include <string>
#include <vector>

#include "math/math.h"
#include "render/rhi_handle.h"

namespace gryce_engine {
namespace assets { class TextureData; }
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
// IRenderer2D — 2D 图形渲染接口
// 支持：矩形、n 边形、圆形、文字
// 所有坐标系以屏幕左上角为原点，向右为 +X，向下为 +Y
// ---------------------------------------------------------------------------
class IRenderer2D {
public:
    virtual ~IRenderer2D() = default;

    // 初始化（必须在 RenderContext::start() 之前调用）
    virtual void init(RenderContext* ctx) = 0;
    virtual void shutdown() = 0;

    // 每帧开始：设置 ortho 投影、blend/depth 状态
    virtual void begin_frame(float screen_width, float screen_height) = 0;
    // 每帧结束：提交所有顶点并绘制
    virtual void end_frame() = 0;

    // -----------------------------------------------------------------------
    // 2D 摄像机（可选）
    // -----------------------------------------------------------------------
    // 设置当前活动摄像机的世界中心坐标与缩放。默认不调用时等价于
    // center=(0,0), zoom=1，左上角为原点。
    virtual void set_camera(const math::Vector2f& center, float zoom) {
        (void)center; (void)zoom;
    }

    // 获取当前摄像机状态（用于 RenderSystem2D 做世界→屏幕变换）
    virtual math::Vector2f camera_center() const { return math::Vector2f::zero(); }
    virtual float camera_zoom() const { return 1.0f; }

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

    // 绘制文字（UTF-8 字符串，当前仅支持 ASCII 可见字符）
    virtual void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) = 0;

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
