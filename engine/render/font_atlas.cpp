#include "font_atlas.h"

#include <algorithm>
#include <cmath>
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

namespace {

// 档位上限：每个档都是整批字形的一份拷贝。6 档足够覆盖"界面文字 + 引擎自带
// 调试文字 + 1/1.5/2 倍 DPI"，再多就是浪费显存（1024² RGBA 已经 4MB）。
constexpr int kMaxBuckets = 6;

// 档位合并容差：请求尺寸与已有档相差 12% 以内就复用该档（1.12 倍缩放的模糊
// 人眼几乎看不出来，却能省掉一次整图重烘）。
constexpr float kSizeTolerance = 0.12f;

float clamp_size(float s) {
    if (s < 4.0f) return 4.0f;
    if (s > 160.0f) return 160.0f;
    return s;
}

} // namespace

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

    sources_.clear();
    std::vector<float> sizes;
    for (const auto& src : sources) {
        if (src.path.empty() || src.ranges.empty()) continue;
        sources_.push_back(src);
        sizes.push_back(clamp_size(src.size));
    }
    if (sources_.empty()) {
        GLOG_ERROR("FontAtlas: no valid font sources loaded");
        return false;
    }
    font_size_ = sizes[0];
    return repack(ctx, sizes);
}

bool FontAtlas::repack(RenderContext* ctx, const std::vector<float>& requested) {
    if (sources_.empty() || requested.empty()) return false;
    if (!ctx) return false;

    // 档位去重：与已接受的档相差 12% 以内就并进去（顺序 = 请求顺序，
    // 第 0 档是兜底档，多参 get_glyph 用它）
    std::vector<float> sizes;
    for (float s : requested) {
        s = clamp_size(s);
        bool dup = false;
        for (float have : sizes) {
            if (std::fabs(s - have) <= kSizeTolerance * std::max(have, 1.0f)) {
                dup = true;
                break;
            }
        }
        if (!dup) sizes.push_back(s);
    }
    if (sizes.empty()) return false;

    // ---- 1. 估图集尺寸：Σ(该档字形数 × 字号²) × 1.2 的平方根，向上取 2 的幂 ----
    long long area = 0;
    for (float s : sizes) {
        for (const auto& src : sources_) {
            for (const auto& range : src.ranges) {
                if (range.last < range.first) continue;
                const long long n = static_cast<long long>(range.last - range.first + 1);
                area += n * static_cast<long long>(s * s * 1.2f);
            }
        }
    }
    int atlas_size = 512;
    while (static_cast<long long>(atlas_size) * atlas_size < area && atlas_size < 4096) {
        atlas_size *= 2;
    }

    // ---- 2. 读字体文件（只读一次，多档复用同一份字节） ----
    struct LoadedFont {
        std::string path;
        std::vector<unsigned char> data;
    };
    std::vector<LoadedFont> fonts;
    fonts.reserve(sources_.size());
    for (const auto& src : sources_) {
        std::ifstream file(src.path, std::ios::binary);
        if (!file) {
            GLOG_ERROR("FontAtlas: failed to open font file: {}", src.path);
            return false;
        }
        LoadedFont font;
        font.path = src.path;
        font.data = std::vector<unsigned char>((std::istreambuf_iterator<char>(file)),
                                               std::istreambuf_iterator<char>());
        file.close();
        fonts.push_back(std::move(font));
    }

    // ---- 3. 打包（放不下就翻倍重来） ----
    struct PackJob {
        int   font_index = 0;
        float size = 0.0f;
        std::vector<stbtt_pack_range> ranges;
        std::vector<std::vector<stbtt_packedchar>> chardata;
    };
    std::vector<PackJob> jobs;
    jobs.reserve(sizes.size() * fonts.size());
    for (float s : sizes) {
        for (std::size_t fi = 0; fi < fonts.size(); ++fi) {
            PackJob job;
            job.font_index = static_cast<int>(fi);
            job.size = s;
            for (const auto& range : sources_[fi].ranges) {
                if (range.last < range.first) continue;
                stbtt_pack_range pr{};
                pr.first_unicode_codepoint_in_range = static_cast<int>(range.first);
                pr.array_of_unicode_codepoints = nullptr;
                pr.num_chars = static_cast<int>(range.last - range.first + 1);
                pr.font_size = s;
                // 2x2 过采样：stb 对 h/v 分别做 2 倍栅格化 + 盒式预滤。原来写成
                // h=2/v=1 只横向滤了一遍，竖笔边缘的灰度台阶因此比横笔更硬（同一
                // 个字横竖笔画质感不一致）。顺带给每个字形留出 1 列 1 行透明守卫
                // 像素（g.width 取 x1-x0 时把这列算进去），缩放采样不啃邻居。
                pr.h_oversample = 2;
                pr.v_oversample = 2;
                pr.chardata_for_range = nullptr;
                job.ranges.push_back(pr);
                job.chardata.emplace_back(pr.num_chars);
                job.ranges.back().chardata_for_range = job.chardata.back().data();
            }
            if (!job.ranges.empty()) jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) {
        GLOG_ERROR("FontAtlas: no glyph ranges provided");
        return false;
    }

    std::vector<unsigned char> bitmap_r8;
    bool packed_ok = false;
    for (;;) {
        bitmap_r8.assign(static_cast<std::size_t>(atlas_size * atlas_size), 0);
        stbtt_pack_context pack_ctx{};
        if (!stbtt_PackBegin(&pack_ctx, bitmap_r8.data(), atlas_size, atlas_size, 0, 1, nullptr)) {
            GLOG_ERROR("FontAtlas: stbtt_PackBegin failed");
            return false;
        }
        // 注：每次重试都要重跑所有 job —— 上一轮的 chardata 会被覆盖，
        // 这正是我们要的（只有成功那轮的坐标有效）。
        packed_ok = true;
        for (auto& job : jobs) {
            const LoadedFont& font = fonts[static_cast<std::size_t>(job.font_index)];
            const int font_offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
            if (!stbtt_PackFontRanges(&pack_ctx, font.data.data(), font_offset,
                                      job.ranges.data(), static_cast<int>(job.ranges.size()))) {
                packed_ok = false;
                break;
            }
        }
        stbtt_PackEnd(&pack_ctx);
        if (packed_ok) break;
        if (atlas_size >= 4096) {
            GLOG_WARN("FontAtlas: {} glyph sizes do not fit in a 4096x4096 atlas", jobs.size());
            return false;
        }
        atlas_size *= 2;
    }

    // ---- 4. 转 RGBA8（白色 + alpha=覆盖度），与 2D sprite 纹理同格式，
    //         避免 NVIDIA 因同一 texture unit 在 R8/RGBA8 间切换而重编 2D shader ----
    std::vector<unsigned char> bitmap_rgba(static_cast<std::size_t>(atlas_size * atlas_size * 4));
    for (std::size_t i = 0; i < bitmap_rgba.size() / 4; ++i) {
        bitmap_rgba[i * 4 + 0] = 255;
        bitmap_rgba[i * 4 + 1] = 255;
        bitmap_rgba[i * 4 + 2] = 255;
        bitmap_rgba[i * 4 + 3] = bitmap_r8[i];
    }

    destroy_texture_only(ctx);
    texture_handle_ = ctx->create_texture();
    texture_ = ctx->texture(texture_handle_);
    if (!texture_handle_.is_valid() || !texture_ ||
        !texture_->upload_data(bitmap_rgba.data(), atlas_size, atlas_size, 4)) {
        GLOG_ERROR("FontAtlas: failed to upload font atlas texture");
        return false;
    }
    atlas_width_ = atlas_size;
    atlas_height_ = atlas_size;

    // ---- 5. 记录字形（按档分开存） ----
    buckets_.clear();
    buckets_.resize(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) buckets_[i].size = sizes[i];
    sizes_ = sizes;

    const float fatlas = static_cast<float>(atlas_size);
    for (const auto& job : jobs) {
        int bucket_index = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (std::fabs(sizes[i] - job.size) < 1e-3f) {
                bucket_index = static_cast<int>(i);
                break;
            }
        }
        auto& glyphs = buckets_[static_cast<std::size_t>(bucket_index)].glyphs;
        for (std::size_t ri = 0; ri < job.ranges.size(); ++ri) {
            const stbtt_pack_range& range = job.ranges[ri];
            const stbtt_packedchar* chars = range.chardata_for_range;
            if (!chars) continue;
            for (int i = 0; i < range.num_chars; ++i) {
                const uint32_t codepoint = static_cast<uint32_t>(range.first_unicode_codepoint_in_range + i);
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

                // 多源合并：先出现的源优先，后面的源只补前面没有的码点
                // （拉丁字体在前、CJK 回退在后时，ASCII 用拉丁字体的字形）
                if (glyphs.find(codepoint) == glyphs.end()) {
                    glyphs[codepoint] = g;
                }
            }
        }
    }

    std::size_t total = 0;
    for (const auto& b : buckets_) total += b.glyphs.size();
    std::string ladder;
    for (float s : sizes_) {
        if (!ladder.empty()) ladder += ", ";
        ladder += std::to_string(static_cast<int>(s + 0.5f));
    }
    GLOG_INFO("FontAtlas: atlas {}x{}, {} glyph(s) over {} size(s) [{}]",
              atlas_size, atlas_size, total, buckets_.size(), ladder);
    return true;
}

bool FontAtlas::ensure_size(RenderContext* ctx, float size) {
    if (sources_.empty()) return false;
    size = clamp_size(size);
    if (has_size(size)) return false;

    std::vector<float> next = sizes_;
    next.push_back(size);
    // 超上限：丢掉最早加进来的非兜底档（第 0 档永远保留——单参 get_glyph 用它）
    while (static_cast<int>(next.size()) > kMaxBuckets) {
        if (next.size() <= 1) break;
        next.erase(next.begin() + 1);
    }
    return repack(ctx, next);
}

bool FontAtlas::has_size(float size, float tolerance) const {
    for (const auto& b : buckets_) {
        if (std::fabs(size - b.size) <= tolerance * std::max(b.size, 1.0f)) return true;
    }
    return false;
}

int FontAtlas::bucket_for(float font_size) const {
    if (buckets_.empty()) return -1;
    int best = 0;
    float best_err = 1e30f;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        // 用相对误差选档：13 画在 32 上是 1.46 倍误差、32 画在 13 上是 1.46，
        // 相对量才对"放大糊"和"缩小虚"一视同仁。
        const float err = std::fabs(font_size - buckets_[i].size) / std::max(buckets_[i].size, 1.0f);
        if (err < best_err) {
            best_err = err;
            best = static_cast<int>(i);
        }
    }
    return best;
}

float FontAtlas::bucket_size(int bucket) const {
    if (bucket < 0 || bucket >= static_cast<int>(buckets_.size())) return font_size_;
    return buckets_[static_cast<std::size_t>(bucket)].size;
}

bool FontAtlas::create_fallback_atlas(RenderContext* ctx, float font_size) {
    destroy(ctx);
    font_size_ = font_size;
    sources_.clear();

    const int atlas_size = 512;
    const int cell_count_x = 16;
    const int cell_size = atlas_size / cell_count_x;                    // 32
    // 字形必须能放进单元格：font_size 过大时 glyph_size 会超过 cell_size，
    // 使 pad 变成负数并产生越界的像素写入（曾导致崩溃），这里钳制到单元格内。
    const int glyph_size = std::max(1, std::min(static_cast<int>(font_size * 0.75f), cell_size));
    const int pad = (cell_size - glyph_size) / 2;

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

    buckets_.clear();
    buckets_.resize(1);
    buckets_[0].size = font_size;
    sizes_.assign(1, font_size);

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

        buckets_[0].glyphs[codepoint] = g;
    }

    GLOG_INFO("Fallback font atlas created ({} glyphs, {}x{})",
              buckets_[0].glyphs.size(), atlas_size, atlas_size);
    return true;
}

const Glyph* FontAtlas::get_glyph(char c) const {
    return get_glyph(static_cast<uint32_t>(static_cast<unsigned char>(c)));
}

const Glyph* FontAtlas::get_glyph(uint32_t codepoint) const {
    return get_glyph(0, codepoint);
}

const Glyph* FontAtlas::get_glyph(int bucket, uint32_t codepoint) const {
    if (bucket < 0 || bucket >= static_cast<int>(buckets_.size())) return nullptr;
    const auto& glyphs = buckets_[static_cast<std::size_t>(bucket)].glyphs;
    auto it = glyphs.find(codepoint);
    if (it == glyphs.end()) return nullptr;
    return &it->second;
}

void FontAtlas::flip_uv_vertical() {
    for (auto& bucket : buckets_) {
        for (auto& [codepoint, g] : bucket.glyphs) {
            (void)codepoint;
            float tmp = g.uv0_y;
            g.uv0_y = g.uv1_y;
            g.uv1_y = tmp;
        }
    }
}

void FontAtlas::destroy(RenderContext* ctx) {
    destroy_texture_only(ctx);
    buckets_.clear();
    sizes_.clear();
    sources_.clear();
    atlas_width_ = 0;
    atlas_height_ = 0;
}

void FontAtlas::destroy_texture_only(RenderContext* ctx) {
    if (texture_handle_.is_valid() && ctx) {
        ctx->destroy_texture(texture_handle_);
    }
    texture_handle_ = RHITextureHandle{};
    texture_ = nullptr;
}

} // namespace gryce_engine::render
