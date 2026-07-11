#pragma once

#include <string>
#include <unordered_map>

#include "math/math.h"
#include "render/rhi_handle.h"

namespace gryce_engine::render {

class ITexture;
class RenderContext;

// ---------------------------------------------------------------------------
// Glyph — 单个字符的字形信息
// ---------------------------------------------------------------------------
struct Glyph {
    float uv0_x = 0.0f, uv0_y = 0.0f;  // 左上角 UV
    float uv1_x = 0.0f, uv1_y = 0.0f;  // 右下角 UV
    float offset_x = 0.0f, offset_y = 0.0f;  // 相对于基线的像素偏移
    float width = 0.0f, height = 0.0f;   // 像素尺寸
    float advance = 0.0f;                // 水平推进（像素）
};

// ---------------------------------------------------------------------------
// FontAtlas — 字体纹理图集
// 使用 stb_truetype 生成，支持 ASCII 可见字符（32-126）
// ---------------------------------------------------------------------------
class FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas();

    // 从字体文件加载并生成 atlas（必须在 RenderContext::start() 之前调用）
    bool init(RenderContext* ctx, const std::string& font_path, float font_size = 32.0f);

    // 系统字体全部失败时使用：生成一个纯色块 fallback atlas，保证文字至少能以色块形式显示
    bool create_fallback_atlas(RenderContext* ctx, float font_size = 32.0f);

    // 获取字符字形（nullptr 表示字符不存在）
    const Glyph* get_glyph(char c) const;

    // 垂直翻转所有 glyph 的 UV（用于 Vulkan：纹理按 top-down 解释，需把 OpenGL bottom-up UV 翻回来）
    void flip_uv_vertical();

    float font_size() const { return font_size_; }
    RHITextureHandle texture_handle() const { return texture_handle_; }
    ITexture* texture() const { return texture_; }

    void destroy(RenderContext* ctx);

private:
    RHITextureHandle texture_handle_;
    ITexture* texture_ = nullptr;
    std::unordered_map<char, Glyph> glyphs_;
    float font_size_ = 32.0f;
    int atlas_width_ = 0;
    int atlas_height_ = 0;
};

} // namespace gryce_engine::render
