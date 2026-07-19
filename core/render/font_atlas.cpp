#include "font_atlas.h"

#include <fstream>
#include <vector>

// 抑制 stb_truetype 中大量旧式 C 风格转换、符号转换和 double-promotion 警告
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#pragma GCC diagnostic pop

#include "render/render_context.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

FontAtlas::~FontAtlas() {}

bool FontAtlas::init(RenderContext* ctx, const std::string& font_path, float font_size) {
    font_size_ = font_size;

    // 读取字体文件
    std::ifstream file(font_path, std::ios::binary);
    if (!file) {
        GLOG_ERROR("Failed to open font file: {}", font_path);
        return false;
    }

    std::vector<unsigned char> ttf_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // .ttc 集合需要找到第一个子字体的偏移；.ttf 返回 0
    int font_offset = stbtt_GetFontOffsetForIndex(ttf_data.data(), 0);
    GLOG_INFO("FontAtlas: loading '{}', offset={}", font_path, font_offset);

    const int atlas_size = 512;
    // 生成 top-down 单通道 R8 位图（行 0 为图像顶部）。
    // Vulkan 直接使用 top-down；OpenGL 上传时会在本地翻转成 bottom-up。
    std::vector<unsigned char> bitmap_r8(static_cast<std::size_t>(atlas_size * atlas_size), 0);
    std::vector<stbtt_bakedchar> cdata(96);

    int result = stbtt_BakeFontBitmap(
        ttf_data.data(), font_offset,
        font_size,
        bitmap_r8.data(), atlas_size, atlas_size,
        32, 96, cdata.data());

    if (result <= 0) {
        GLOG_ERROR("stbtt_BakeFontBitmap failed: {}", result);
        return false;
    }

    // 创建纹理（RGBA8，alpha 来自字形覆盖度，颜色设为白色）。
    // 与 2D sprite 纹理格式保持一致，避免 NVIDIA 因同一 texture unit
    // 在 R8/RGBA8 之间切换而重新编译 2D shader。
    std::vector<unsigned char> bitmap_rgba(static_cast<std::size_t>(atlas_size * atlas_size * 4));
    for (std::size_t i = 0; i < bitmap_rgba.size() / 4; ++i) {
        unsigned char a = bitmap_r8[i];
        bitmap_rgba[i * 4 + 0] = 255;
        bitmap_rgba[i * 4 + 1] = 255;
        bitmap_rgba[i * 4 + 2] = 255;
        bitmap_rgba[i * 4 + 3] = a;
    }

    texture_handle_ = ctx->create_texture();
    texture_ = ctx->texture(texture_handle_);
    if (!texture_handle_.is_valid() || !texture_ ||
        !texture_->upload_data(bitmap_rgba.data(), atlas_size, atlas_size, 4)) {
        GLOG_ERROR("Failed to upload font atlas texture");
        return false;
    }

    atlas_width_ = atlas_size;
    atlas_height_ = atlas_size;

    // 记录每个字符的 glyph 信息。
    // stbtt_BakeFontBitmap 返回的 y0/y1 是相对于 top-down 位图（y0 为顶部）。
    // 位图以 top-down 上传，因此 UV 直接使用 top-down 坐标：
    //   uv0_y = y0/atlas（顶部），uv1_y = y1/atlas（底部）。
    // 屏幕 Y 向下增长，quad 顶部使用 uv0_y，底部使用 uv1_y，字形即正向显示。
    const float fatlas = static_cast<float>(atlas_size);
    for (size_t i = 0; i < 96; ++i) {
        char c = static_cast<char>(i + 32);
        const stbtt_bakedchar& b = cdata[i];

        Glyph g;
        g.uv0_x = b.x0 / fatlas;
        g.uv0_y = b.y0 / fatlas;
        g.uv1_x = b.x1 / fatlas;
        g.uv1_y = b.y1 / fatlas;
        g.offset_x = b.xoff;
        g.offset_y = b.yoff;
        g.width = static_cast<float>(b.x1 - b.x0);
        g.height = static_cast<float>(b.y1 - b.y0);
        g.advance = b.xadvance;

        glyphs_[c] = g;
    }

    GLOG_INFO("Font atlas created: {} ({} glyphs, {}x{})", font_path, glyphs_.size(), atlas_size, atlas_size);
    return true;
}

bool FontAtlas::create_fallback_atlas(RenderContext* ctx, float font_size) {
    font_size_ = font_size;
    const int atlas_size = 512;
    const int cell_count_x = 16;
    const int cell_size = atlas_size / cell_count_x;                    // 32
    const int glyph_size = static_cast<int>(font_size * 0.75f);         // ~24
    const int pad = (cell_size - glyph_size) / 2;                       // 4

    // fallback 同样使用 RGBA8 top-down，保持与正常字体一致的采样方式（采样 .a）
    std::vector<unsigned char> bitmap_rgba(static_cast<std::size_t>(atlas_size * atlas_size * 4), 0);
    for (int i = 0; i < 96; ++i) {
        int cx = i % cell_count_x;
        int cy = i / cell_count_x;
        int base_x = cx * cell_size + pad;
        int base_y = cy * cell_size + pad;
        for (int y = 0; y < glyph_size; ++y) {
            for (int x = 0; x < glyph_size; ++x) {
                int px = base_x + x;
                int py = base_y + y;
                std::size_t dst = static_cast<std::size_t>((py * atlas_size + px) * 4);
                bitmap_rgba[dst + 0] = 255;
                bitmap_rgba[dst + 1] = 255;
                bitmap_rgba[dst + 2] = 255;
                bitmap_rgba[dst + 3] = 255;
            }
        }
    }

    texture_handle_ = ctx->create_texture();
    texture_ = ctx->texture(texture_handle_);
    if (!texture_handle_.is_valid() || !texture_ ||
        !texture_->upload_data(bitmap_rgba.data(), atlas_size, atlas_size, 4)) {
        GLOG_ERROR("Failed to upload fallback font atlas texture");
        return false;
    }

    atlas_width_ = atlas_size;
    atlas_height_ = atlas_size;

    for (int i = 0; i < 96; ++i) {
        char c = static_cast<char>(i + 32);
        int cx = i % cell_count_x;
        int cy = i / cell_count_x;

        const float fx = static_cast<float>(cx * cell_size + pad);
        const float fy = static_cast<float>(cy * cell_size + pad);
        const float fsize = static_cast<float>(glyph_size);
        const float fcell = static_cast<float>(cell_size);
        const float fatlas = static_cast<float>(atlas_size);

        Glyph g;
        // fallback 位图按 top-down 生成，UV 直接使用 top-down 坐标
        g.uv0_x = fx / fatlas;
        g.uv0_y = fy / fatlas;
        g.uv1_x = (fx + fsize) / fatlas;
        g.uv1_y = (fy + fsize) / fatlas;
        g.offset_x = static_cast<float>(pad);
        g.offset_y = -static_cast<float>(pad + glyph_size);
        g.width = fsize;
        g.height = fsize;
        g.advance = fcell;

        if (c == ' ') {
            g.width = 0.0f;
            g.height = 0.0f;
            g.advance = fcell / 2.0f;
        }

        glyphs_[c] = g;
    }

    GLOG_INFO("Fallback font atlas created ({} glyphs, {}x{})", glyphs_.size(), atlas_size, atlas_size);
    return true;
}

const Glyph* FontAtlas::get_glyph(char c) const {
    auto it = glyphs_.find(c);
    if (it == glyphs_.end()) return nullptr;
    return &it->second;
}

void FontAtlas::flip_uv_vertical() {
    for (auto& [c, g] : glyphs_) {
        g.uv0_y = 1.0f - g.uv0_y;
        g.uv1_y = 1.0f - g.uv1_y;
    }
}

void FontAtlas::destroy(RenderContext* ctx) {
    if (texture_handle_.is_valid()) {
        ctx->destroy_texture(texture_handle_);
        texture_handle_ = RHITextureHandle{};
        texture_ = nullptr;
    }
    glyphs_.clear();
}

} // namespace gryce_engine::render
