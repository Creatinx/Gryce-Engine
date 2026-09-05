#include "font_atlas.h"

#include <algorithm>
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
    FontSource src;
    src.path = font_path;
    src.size = font_size;
    src.ranges.push_back({32, 126}); // ASCII 可见字符
    return init_multi(ctx, {src});
}

bool FontAtlas::init_multi(RenderContext* ctx, const std::vector<FontSource>& sources) {
    if (sources.empty()) return false;

    destroy(ctx);
    glyphs_.clear();

    font_size_ = sources[0].size;

    // 统计总字形数与最大字号，用于估算 atlas 尺寸
    int total_glyphs = 0;
    float max_size = 0.0f;
    for (const auto& src : sources) {
        for (const auto& range : src.ranges) {
            if (range.last >= range.first) {
                total_glyphs += static_cast<int>(range.last - range.first + 1);
            }
        }
        max_size = std::max(max_size, src.size);
    }

    if (total_glyphs == 0) {
        GLOG_ERROR("FontAtlas: no glyph ranges provided");
        return false;
    }

    // 估算 atlas 尺寸：每个字形按字号平方留出 1.2 倍余量
    int atlas_size = 512;
    while (atlas_size * atlas_size < static_cast<int>(total_glyphs * max_size * max_size * 1.2f)) {
        atlas_size *= 2;
        if (atlas_size > 4096) break;
    }

    // 加载每个字体源的数据并构造 stbtt_pack_range
    struct LoadedFont {
        std::string path;
        std::vector<unsigned char> data;
        std::vector<stbtt_pack_range> pack_ranges;
        std::vector<std::vector<stbtt_packedchar>> chardata;
    };
    std::vector<LoadedFont> fonts;
    fonts.reserve(sources.size());

    for (const auto& src : sources) {
        LoadedFont font;
        font.path = src.path;

        std::ifstream file(src.path, std::ios::binary);
        if (!file) {
            GLOG_ERROR("FontAtlas: failed to open font file: {}", src.path);
            return false;
        }
        font.data = std::vector<unsigned char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        for (const auto& range : src.ranges) {
            if (range.last < range.first) continue;

            stbtt_pack_range pr{};
            pr.first_unicode_codepoint_in_range = static_cast<int>(range.first);
            pr.array_of_unicode_codepoints = nullptr;
            pr.num_chars = static_cast<int>(range.last - range.first + 1);
            pr.font_size = src.size;
            pr.h_oversample = 2;
            pr.v_oversample = 1;
            pr.chardata_for_range = nullptr;

            font.pack_ranges.push_back(pr);
            font.chardata.emplace_back(pr.num_chars);
            font.pack_ranges.back().chardata_for_range = font.chardata.back().data();
        }

        if (!font.pack_ranges.empty()) {
            fonts.push_back(std::move(font));
        }
    }

    if (fonts.empty()) {
        GLOG_ERROR("FontAtlas: no valid font sources loaded");
        return false;
    }

    // 打包到单通道 R8 位图
    std::vector<unsigned char> bitmap_r8(static_cast<std::size_t>(atlas_size * atlas_size), 0);
    stbtt_pack_context pack_ctx{};
    if (!stbtt_PackBegin(&pack_ctx, bitmap_r8.data(), atlas_size, atlas_size, 0, 1, nullptr)) {
        GLOG_ERROR("FontAtlas: stbtt_PackBegin failed");
        return false;
    }

    for (auto& font : fonts) {
        int font_offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
        if (!stbtt_PackFontRanges(&pack_ctx, font.data.data(), font_offset,
                                  font.pack_ranges.data(),
                                  static_cast<int>(font.pack_ranges.size()))) {
            GLOG_WARN("FontAtlas: failed to pack some ranges from '{}'", font.path);
        }
    }

    stbtt_PackEnd(&pack_ctx);

    // 转换为 RGBA8，alpha 存储字形覆盖度，颜色设为白色
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
        GLOG_ERROR("FontAtlas: failed to upload font atlas texture");
        return false;
    }

    atlas_width_ = atlas_size;
    atlas_height_ = atlas_size;

    // 记录每个字符的字形信息
    const float fatlas = static_cast<float>(atlas_size);
    for (const auto& font : fonts) {
        for (std::size_t ri = 0; ri < font.pack_ranges.size(); ++ri) {
            const stbtt_pack_range& range = font.pack_ranges[ri];
            const stbtt_packedchar* chars = range.chardata_for_range;
            for (int i = 0; i < range.num_chars; ++i) {
                uint32_t codepoint = static_cast<uint32_t>(range.first_unicode_codepoint_in_range + i);
                const stbtt_packedchar& b = chars[i];

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

                glyphs_[codepoint] = g;
            }
        }
    }

    GLOG_INFO("FontAtlas: created multi-source atlas ({} glyphs, {}x{})",
              glyphs_.size(), atlas_size, atlas_size);
    return true;
}

bool FontAtlas::create_fallback_atlas(RenderContext* ctx, float font_size) {
    destroy(ctx);
    glyphs_.clear();
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
        uint32_t codepoint = static_cast<uint32_t>(i + 32);
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

        if (codepoint == ' ') {
            g.width = 0.0f;
            g.height = 0.0f;
            g.advance = fcell / 2.0f;
        }

        glyphs_[codepoint] = g;
    }

    GLOG_INFO("Fallback font atlas created ({} glyphs, {}x{})", glyphs_.size(), atlas_size, atlas_size);
    return true;
}

const Glyph* FontAtlas::get_glyph(char c) const {
    return get_glyph(static_cast<uint32_t>(static_cast<unsigned char>(c)));
}

const Glyph* FontAtlas::get_glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    if (it == glyphs_.end()) return nullptr;
    return &it->second;
}

void FontAtlas::flip_uv_vertical() {
    for (auto& [codepoint, g] : glyphs_) {
        (void)codepoint;
        float tmp = g.uv0_y;
        g.uv0_y = g.uv1_y;
        g.uv1_y = tmp;
    }
}

void FontAtlas::destroy(RenderContext* ctx) {
    if (texture_handle_.is_valid()) {
        ctx->destroy_texture(texture_handle_);
        texture_handle_ = RHITextureHandle{};
        texture_ = nullptr;
    }
    glyphs_.clear();
    atlas_width_ = 0;
    atlas_height_ = 0;
}

} // namespace gryce_engine::render
