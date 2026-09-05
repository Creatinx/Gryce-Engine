#pragma once

// UIRenderer — 专用 UI 渲染管线
//
// 提供：
// 1. 专用 UI 着色器（支持圆角矩形 SDF、字体图集、纯色填充）
// 2. GenerateMesh() 将控件树顶点化到单一 buffer
// 3. 正确的 OpenGL 状态管理（关闭深度测试、开启混合、专用正交投影）
//
// 坐标系统：屏幕左上角为原点，向右为 +X，向下为 +Y

#include <cstdint>
#include <string>
#include <vector>

#include "GryceEngineUtils/ui/widget.h"
#include "render/render_context.h"
#include "render/font_atlas.h"
#include "render/rhi_handle.h"
#include "render/mesh.h"
#include "render/shader.h"

namespace gryce_engine::render { class RenderContext; class ITexture; }

namespace GryceEngineUtils::ui {

// ---------------------------------------------------------------------------
// UIVertex — UI 专用顶点格式
// 位置(2) + 颜色(4) + 纹理坐标(2) + 模式(1) + 矩形 SDF 数据(5) = 14 floats
// ---------------------------------------------------------------------------
struct UIVertex {
    float x, y;           // 屏幕空间位置
    float r, g, b, a;     // 颜色
    float u, v;           // 纹理坐标（字体图集 UV 或片段位置）
    float mode;           // 0=纯色填充, 1=字体图集, 2=圆角矩形 SDF
    float rx, ry;         // 矩形原点（SDF 用）
    float rw, rh;         // 矩形尺寸（SDF 用）
    float radius;         // 圆角半径（SDF 用）
};
// 总大小: 14 * 4 = 56 bytes

// ---------------------------------------------------------------------------
// UIRenderer — 专用 UI 渲染器
// ---------------------------------------------------------------------------
class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    // 初始化：编译着色器、创建字体图集、创建 mesh
    bool init(gryce_engine::render::RenderContext* ctx);
    void shutdown();
    bool initialized() const { return initialized_; }

    // 每帧开始/结束：设置状态 + 提交绘制
    void begin_frame(float screen_w, float screen_h);
    void end_frame();

    // GenerateMesh — 遍历控件树生成顶点
    // 替代原有的逐个 Widget::draw() 调用
    void generate_mesh(Widget* root, const std::vector<Widget*>& modals);

    // 绘制原语（GenerateMesh 遍历时调用）
    void draw_rect(float x, float y, float w, float h, const Color& color);
    void draw_rounded_rect(float x, float y, float w, float h,
                           float radius, const Color& color);
    void draw_text(float x, float y, const std::string& text,
                   float font_size, const Color& color);
    void draw_focus_ring(float x, float y, float w, float h,
                         float radius, const Color& color);

    // 裁剪（ScrollView 用）
    void set_scissor(int x, int y, int w, int h);
    void reset_scissor();

    // 访问字体图集纹理句柄
    gryce_engine::render::RHITextureHandle font_texture_handle() const {
        return font_atlas_ ? font_atlas_->texture_handle() : gryce_engine::render::RHITextureHandle{};
    }

private:
    void flush();
    void push_quad(float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1,
                   const Color& color, float mode,
                   float rx, float ry, float rw, float rh, float radius);
    void ensure_capacity(size_t extra_verts, size_t extra_indices);

    // 绘制一个控件的背景 + 文字 + 焦点环
    void draw_widget_mesh(Widget* widget);

    // 辅助：文本宽度估算
    float text_width(const std::string& s, float font_size) const;

    gryce_engine::render::RenderContext* ctx_ = nullptr;
    gryce_engine::render::RHIShaderHandle shader_;
    gryce_engine::render::RHIMeshHandle mesh_;
    gryce_engine::render::FontAtlas* font_atlas_ = nullptr;

    std::vector<UIVertex> vertices_;
    std::vector<uint32_t> indices_;
    float screen_w_ = 0.0f;
    float screen_h_ = 0.0f;
    math::Matrix4f ortho_;
    bool initialized_ = false;
    bool in_frame_ = false;
    int scissor_x_ = 0, scissor_y_ = 0, scissor_w_ = 0, scissor_h_ = 0;
    bool scissor_active_ = false;
};

} // namespace GryceEngineUtils::ui