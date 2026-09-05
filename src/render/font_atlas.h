#pragma once

#include "export.h"

#include <string>
#include <unordered_map>
#include <vector>

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
// FontRange — Unicode 字符范围（包含边界）
// ---------------------------------------------------------------------------
struct FontRange {
    uint32_t first = 0;
    uint32_t last = 0;
};

// ---------------------------------------------------------------------------
// FontSource — 单个字体源及其需要打包的字符范围
// ---------------------------------------------------------------------------
struct FontSource {
    std::string path;
    float size = 32.0f;
    std::vector<FontRange> ranges;
};

// ---------------------------------------------------------------------------
// FontAtlas — 字体纹理图集
// 使用 stb_truetype 生成，支持多字体源合并与任意 Unicode 范围
// ---------------------------------------------------------------------------
class GRYCE_API FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas();

    // 从单个字体文件加载并生成 atlas（ASCII 可见字符 32-126）
    bool init(RenderContext* ctx, const std::string& font_path, float font_size = 32.0f);

    // 从多个字体源加载并合并到同一个 atlas（例如 Latin + CJK 回退）
    bool init_multi(RenderContext* ctx, const std::vector<FontSource>& sources);

    // 系统字体全部失败时使用：生成一个纯色块 fallback atlas，保证文字至少能以色块形式显示
    bool create_fallback_atlas(RenderContext* ctx, float font_size = 32.0f);

    // 获取字符字形（nullptr 表示字符不存在）
    const Glyph* get_glyph(char c) const;
    const Glyph* get_glyph(uint32_t codepoint) const;

    // 垂直翻转所有 glyph 的 UV（用于 Vulkan：纹理按 top-down 解释，需把 OpenGL bottom-up UV 翻回来）
    void flip_uv_vertical();

    float font_size() const { return font_size_; }
    RHITextureHandle texture_handle() const { return texture_handle_; }
    ITexture* texture() const { return texture_; }
    int atlas_width() const { return atlas_width_; }
    int atlas_height() const { return atlas_height_; }
    const std::unordered_map<uint32_t, Glyph>& glyphs() const { return glyphs_; }

    void destroy(RenderContext* ctx);

private:
    RHITextureHandle texture_handle_;
    ITexture* texture_ = nullptr;
    std::unordered_map<uint32_t, Glyph> glyphs_;
    float font_size_ = 32.0f;
    int atlas_width_ = 0;
    int atlas_height_ = 0;
};

} // namespace gryce_engine::render
