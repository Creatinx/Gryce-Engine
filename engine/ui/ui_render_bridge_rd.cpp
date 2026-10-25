// ===========================================================================
// engine/ui/ui_render_bridge_rd.cpp — UiRenderer 的 IRenderer2D 适配器
//
// 定位：引擎内**唯一**一处把 UI 模型层（ui_drawlist / ui_render_bridge）接到
// 渲染器的地方。适配器存在的理由是两边有三种"方言"要翻译：
//   1) 颜色：drawlist/主题用 0xRRGGBBAA（uint32），IRenderer2D 用 float 0~1；
//   2) 裁剪：桥给 float 矩形，IRenderer2D::set_scissor 是 int——向**外**取整
//      （地板 x/y、天花板 x+w/y+h），保证裁剪只少不多，不会啃掉内容的边缘；
//   3) 文本垂直基准：drawlist 的 Text 命令 (x,y) 是行盒左上角（与 ImGui 的
//      惯例一致，控件层按行盒布局），而 Renderer2D::draw_text 的 y 是**基线**
//      （字形按 offset_y 相对基线摆放，见 font_atlas.cpp 的 b.yoff）。适配器
//      按升部比把行盒顶换算成基线。
//   4) 圆角：模型层的角半径/线宽/阴影扩散都是**逻辑像素**，与左上角坐标同一
//      个口径，所以缩放时一起乘——只乘位置会让 2 倍屏上的圆角看起来更尖。
//
// 升部比是近似值：引擎没有公开"字体升部"查询接口（Renderer2D 的 font_atlas_
// 是私有的），而典型拉丁字体（含 fallback 图集）的 ascent/em 在 0.8 左右。
// 偏差只影响文本垂直位置（<=0.1em，十几像素字号下 1~2 像素），不影响命中
// 测试——控件层画文字用的就是同一套近似度量，交互矩形始终比文本大。
// 等引擎补上字体度量 API 后，把 kAscentRatio 换成真实值即可，接口不变。
//
// 本文件只编入 GryceRenderer（GryceUiTests 不编：测试用 mock 渲染器，
// 不链接任何引擎 DLL）。
// ===========================================================================

#include "ui/ui_render_bridge.h"

#include <cmath>
#include <memory>
#include <vector>

#include "render/render2d.h"
#include "render/rhi_handle.h"

namespace gryce_ui {

namespace {

// 行盒顶 → 基线的换算比例（ascent/em），见文件头。
constexpr float kAscentRatio = 0.8f;

gryce_engine::render::Color to_color(uint32_t rgba) {
    return gryce_engine::render::Color(
        static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgba & 0xFFu) / 255.0f);
}

// uint64 纹理 id ↔ RHITextureHandle：低 32 位 index、高 32 位 generation。
// 与 ui_api.cpp 里 GUi_Image 的打包方式一致（那边是 imgui 纹理 id，这边是
// 引擎纹理句柄，同一个约定便于宿主把两种 id 都存进 UiCommand.texture）。
gryce_engine::render::RHITextureHandle to_handle(uint64_t packed) {
    gryce_engine::render::RHITextureHandle h;
    h.index      = static_cast<uint32_t>(packed & 0xFFFFFFFFull);
    h.generation = static_cast<uint32_t>((packed >> 32) & 0xFFFFFFFFull);
    return h;
}

class Renderer2DAdapter final : public UiRenderer {
public:
    Renderer2DAdapter(gryce_engine::render::IRenderer2D* renderer, float scale)
        : renderer_(renderer), scale_((scale > 0.0f) ? scale : 1.0f) {}

    void ui_draw_rect(float x, float y, float w, float h, uint32_t rgba) override {
        if (!renderer_) return;
        renderer_->draw_rect(x * scale_, y * scale_, w * scale_, h * scale_,
                             to_color(rgba));
    }

    // 圆角族：半径、线宽同样是逻辑像素，跟着一起放大——否则 2 倍屏上圆角
    // 只有视觉上的一半，"圆角在缩放后变尖"就是这么来的。
    void ui_draw_round_rect(float x, float y, float w, float h,
                            const float radii[4], uint32_t rgba) override {
        if (!renderer_) return;
        float r[4];
        scaled_radii(radii, r);
        renderer_->draw_round_rect(x * scale_, y * scale_, w * scale_, h * scale_,
                                   r, to_color(rgba));
    }

    void ui_draw_round_gradient(float x, float y, float w, float h,
                                const float radii[4], uint32_t top,
                                uint32_t bottom) override {
        if (!renderer_) return;
        float r[4];
        scaled_radii(radii, r);
        renderer_->draw_round_rect_gradient(x * scale_, y * scale_, w * scale_,
                                            h * scale_, r, to_color(top),
                                            to_color(bottom));
    }

    void ui_draw_round_border(float x, float y, float w, float h,
                              const float radii[4], float thickness,
                              uint32_t rgba) override {
        if (!renderer_) return;
        float r[4];
        scaled_radii(radii, r);
        renderer_->draw_round_rect_border(x * scale_, y * scale_, w * scale_, h * scale_,
                                          r, thickness * scale_, to_color(rgba));
    }

    void ui_draw_shadow(float x, float y, float w, float h, const float radii[4],
                        float spread, uint32_t rgba) override {
        if (!renderer_) return;
        float r[4];
        scaled_radii(radii, r);
        renderer_->draw_round_shadow(x * scale_, y * scale_, w * scale_, h * scale_,
                                     r, spread * scale_, to_color(rgba));
    }

    void ui_draw_polygon(const float* xy, int count, uint32_t rgba) override {
        if (!renderer_ || !xy || count < 3) return;
        points_.clear();
        points_.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            points_.emplace_back(xy[i * 2] * scale_, xy[i * 2 + 1] * scale_);
        }
        renderer_->draw_polygon(points_, to_color(rgba));
    }

    void ui_draw_text(float x, float y, const char* text, float font_size,
                      uint32_t rgba) override {
        if (!renderer_ || !text) return;
        // 行盒顶 → 基线（见文件头第 3 条）；字号取整到物理像素（与度量同一
        // 口径，见 ui_text_physical_size），再和坐标一起按缩放放大
        const float fs = ui_text_physical_size(font_size, scale_);
        renderer_->draw_text(x * scale_, y * scale_ + fs * kAscentRatio, text, fs,
                             to_color(rgba));
    }

    void ui_draw_sprite(float x, float y, float w, float h, uint64_t texture,
                        float u0, float v0, float u1, float v1, uint32_t rgba) override {
        if (!renderer_) return;
        renderer_->draw_sprite_region(x * scale_, y * scale_, w * scale_, h * scale_,
                                      u0, v0, u1, v1, to_handle(texture), to_color(rgba));
    }

    void ui_set_scissor(float x, float y, float w, float h) override {
        if (!renderer_) return;
        // float → int 向外取整：裁剪域只允许变大，不允许变小。
        // y 要翻：引擎的 scissor 是 GL 约定（原点在左下），UI 的矩形原点在
        // 左上。不翻的话只要有裁剪域不在画面正中就会被切到错的位置。
        const float sx = x * scale_;
        const float sy = y * scale_;
        const float ex = sx + w * scale_;
        const float ey = sy + h * scale_;
        const int ix = static_cast<int>(std::floor(sx));
        const int iw = static_cast<int>(std::ceil(ex)) - ix;
        const int ih = static_cast<int>(std::ceil(ey)) - static_cast<int>(std::floor(sy));
        const float vh = renderer_->screen_size().y;
        const int iy = static_cast<int>(std::floor(vh - ey));
        renderer_->set_scissor(ix, iy, iw, ih);
    }

    void ui_reset_scissor() override {
        if (!renderer_) return;
        renderer_->reset_scissor();
    }

private:
    void scaled_radii(const float radii[4], float out[4]) const {
        for (int i = 0; i < 4; ++i) {
            out[i] = (radii ? radii[i] : 0.0f) * scale_;
        }
    }

    gryce_engine::render::IRenderer2D* renderer_;  // 不持有：生命周期归宿主
    float scale_ = 1.0f;
    // 多边形顶点复用缓冲：勾/箭头每帧几十个，避免逐个 emplace 反复分配
    std::vector<gryce_engine::math::Vector2f> points_;
};

} // namespace

std::unique_ptr<UiRenderer> make_renderer2d_ui_renderer(
    gryce_engine::render::IRenderer2D* renderer, float scale) {
    return std::make_unique<Renderer2DAdapter>(renderer, scale);
}

} // namespace gryce_ui
