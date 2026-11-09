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

// 档位上限：每个档都是整批字形的一份拷贝。8 档够覆盖"界面正文字号 + 标题
// + 引擎自带调试文字 + 1/1.5/2 倍 DPI"，再多就是浪费显存（4096² RGBA 已 64MB）。
constexpr int kMaxBuckets = 8;

// 档位合并容差：请求尺寸与已有档相差 12% 以内就复用该档（1.12 倍缩放的模糊
// 人眼几乎看不出来，却能省掉一次整图重烘）。
constexpr float kSizeTolerance = 0.12f;

// 字形栅格化的过采样倍率（stb 上限 8）。1 = 二值位图，抗锯齿全靠采样插值；
// 8x8 = 每像素 64 个子样本，覆盖率量化到 1/64，边缘才有真正的灰度过渡。
constexpr int kOversample = 8;

// 覆盖率曲线指数。16px 等宽字体的竖笔本就只有一个像素宽，8x8 过采样量到的
// 笔画芯部覆盖率通常只有 60%~75%，再叠上目标色就显灰、显软。小于 1 的指数
// 把整条曲线往上抬，笔画芯部压向实心，边缘仍保留渐变。
constexpr float kCoverageGamma = 0.68f;

// 笔画加粗（stem darkening）强度。按四邻域取最大覆盖率再乘这个系数，取 max
// 并入原值：只在笔画外侧补一圈半透明，不会把已经实心的像素变淡。0 = 关闭。
constexpr float kStemGain = 0.56f;

// 整形强度随字号衰减：字号越小，单个像素占笔画的比例越高、覆盖率越容易偏灰，
// 需要满强度；到 24px 笔画已经占满几个像素，再抬曲线只会让字变胖发糊，直接
// 关闭。中间线性过渡，避免同一份界面里小字过实、大字发虚。
constexpr float kShapeFullPx = 14.0f;
constexpr float kShapeNonePx = 24.0f;

float clamp_size(float s) {
    if (s < 4.0f) return 4.0f;
    if (s > 160.0f) return 160.0f;
    return s;
}

// 运行时可调的字形整形强度，见 FontAtlas::set_shape_strength
float g_shape_strength = 1.0f;

// 档位尺寸 / 逻辑字号（超采样时为 2），以及"整形参数变了要重烘"的脏标记
float g_shape_logical_scale = 1.0f;
bool g_shape_dirty = false;

} // namespace

void FontAtlas::set_shape_strength(float strength) {
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 2.5f) strength = 2.5f;
    g_shape_strength = strength;
}

float FontAtlas::shape_strength() {
    return g_shape_strength;
}

void FontAtlas::set_shape_logical_scale(float scale) {
    if (!(scale > 1.0f)) scale = 1.0f;
    if (std::fabs(scale - g_shape_logical_scale) < 1e-3f) return;
    g_shape_logical_scale = scale;
    g_shape_dirty = true;
}

bool FontAtlas::reshape_if_dirty(RenderContext* ctx) {
    if (!g_shape_dirty || sizes_.empty()) return false;
    g_shape_dirty = false;
    return repack(ctx, sizes_);
}

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
        // 过采样必须设在 **pack context** 上：stbtt_PackFontRanges 开头会用
        // spc->h_oversample 覆写每个 range 里的同名字段（注释也写明 "don't set
        // these"），所以直接改 stbtt_pack_range 是静默无效的 —— 之前就是这么
        // 写的，图集里的字形一直是 1 倍二值位图，屏幕上的"平滑边缘"其实全是
        // 采样时的双线性插值糊出来的，不是真正的覆盖率抗锯齿。
        // 8x8 是 stb 上限：覆盖率量化 1/(8*8)=65 级，斜笔画/圆角才真的连续。
        stbtt_PackSetOversampling(&pack_ctx, kOversample, kOversample);
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

    // ---- 3.5 覆盖率整形：抬曲线 + 轻度加粗 ----
    // 过采样只保证"边缘有正确的灰度"，不保证"笔画够实"。等宽字体在 16px 下
    // 竖笔 1 个像素宽、横笔两个像素各半覆盖，直接画出来就是一层灰纱。这里在
    // 烘焙期一次性整形：曲线抬陡让芯部接近实心，四邻域膨胀补足 1px 笔画的
    // 侧向覆盖。膨胀严格限制在字形自身的图集矩形内，不会啃到相邻字形。
    {
        std::vector<unsigned char> shaped = bitmap_r8;
        const std::size_t stride = static_cast<std::size_t>(atlas_size);
        unsigned char lut[256];
        for (const auto& job : jobs) {
            if (g_shape_strength <= 0.0f) break;
            // 整形强度按**逻辑字号**判断：超采样时 32 档对应的仍是 16px 的字，
            // 若按 32 判就会掉进"大字号不整形"，笔画反而比 1x 渲染更细。
            const float logical_size = job.size / std::max(1.0f, g_shape_logical_scale);
            const float t = std::clamp((kShapeNonePx - logical_size) / (kShapeNonePx - kShapeFullPx),
                                       0.0f, 1.0f);
            if (t <= 0.0f) continue;
            const float amount = std::min(1.5f, t * g_shape_strength);
            const float gamma = std::max(0.40f, 1.0f - amount * (1.0f - kCoverageGamma));
            const float stem = std::min(1.0f, kStemGain * amount);
            for (int i = 0; i < 256; ++i) {
                const float c = static_cast<float>(i) / 255.0f;
                lut[i] = static_cast<unsigned char>(std::pow(c, gamma) * 255.0f + 0.5f);
            }
            for (const auto& range : job.ranges) {
                const stbtt_packedchar* chars = range.chardata_for_range;
                if (!chars) continue;
                for (int ci = 0; ci < range.num_chars; ++ci) {
                    const stbtt_packedchar& b = chars[ci];
                    const int bx0 = static_cast<int>(b.x0);
                    const int bx1 = static_cast<int>(b.x1);
                    const int by0 = static_cast<int>(b.y0);
                    const int by1 = static_cast<int>(b.y1);
                    if (bx1 <= bx0 || by1 <= by0) continue;
                    for (int y = by0; y < by1; ++y) {
                        for (int x = bx0; x < bx1; ++x) {
                            const std::size_t idx = static_cast<std::size_t>(y) * stride +
                                                    static_cast<std::size_t>(x);
                            unsigned int neighbour = 0;
                            if (x > bx0) neighbour = std::max(neighbour, static_cast<unsigned int>(lut[bitmap_r8[idx - 1]]));
                            if (x + 1 < bx1) neighbour = std::max(neighbour, static_cast<unsigned int>(lut[bitmap_r8[idx + 1]]));
                            if (y > by0) neighbour = std::max(neighbour, static_cast<unsigned int>(lut[bitmap_r8[idx - stride]]));
                            if (y + 1 < by1) neighbour = std::max(neighbour, static_cast<unsigned int>(lut[bitmap_r8[idx + stride]]));
                            const unsigned int self = lut[bitmap_r8[idx]];
                            const unsigned int gained = static_cast<unsigned int>(
                                static_cast<float>(neighbour) * stem + 0.5f);
                            shaped[idx] = static_cast<unsigned char>(
                                std::min(255u, std::max(self, gained)));
                        }
                    }
                }
            }
        }
        bitmap_r8.swap(shaped);
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
                // 逻辑宽高必须用 xoff2-xoff：过采样时 x0/x1 是图集里放大
                // oversample 倍后的像素矩形，直接拿来当绘制尺寸会让字形胀大
                // 8 倍且 UV 只覆盖 1/8（越界采样到邻居字形）。stb 已经把
                // xoff/xoff2 按 1/oversample 换算回逻辑像素。
                g.width = b.xoff2 - b.xoff;
                g.height = b.yoff2 - b.yoff;
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
    for (auto& b : buckets_) {
        // 大写高决定文字视觉重心，'H' 在所有等宽/无衬线字体里都齐备。
        const auto cap = b.glyphs.find(static_cast<uint32_t>('H'));
        b.cap_height = (cap != b.glyphs.end() && cap->second.height > 0.0f)
                           ? cap->second.height
                           : b.size * 0.72f;
        total += b.glyphs.size();
    }
    std::string ladder;
    for (float s : sizes_) {
        if (!ladder.empty()) ladder += ", ";
        ladder += std::to_string(static_cast<int>(s + 0.5f));
    }
    GLOG_INFO("FontAtlas: atlas {}x{}, {} glyph(s) over {} size(s) [{}]",
              atlas_size, atlas_size, total, buckets_.size(), ladder);
    return true;
}

namespace {

// 档位超上限时淘汰"与邻居最挤"的一档：两档尺寸只差几像素时，留一档就够
// （缩放比接近 1），把名额让给真正缺的尺寸。
void prune_buckets(std::vector<float>& sizes, int max_buckets) {
    while (static_cast<int>(sizes.size()) > max_buckets && sizes.size() > 1) {
        int victim = 1;
        float tightest = 1e30f;
        for (int i = 1; i < static_cast<int>(sizes.size()); ++i) {
            const float lo = sizes[static_cast<std::size_t>(i)] - sizes[static_cast<std::size_t>(i - 1)];
            const float hi = (i + 1 < static_cast<int>(sizes.size()))
                                 ? sizes[static_cast<std::size_t>(i + 1)] - sizes[static_cast<std::size_t>(i)]
                                 : lo;
            const float gap = std::min(lo, hi);
            if (gap < tightest) {
                tightest = gap;
                victim = i;
            }
        }
        sizes.erase(sizes.begin() + victim);
    }
}

} // namespace

bool FontAtlas::ensure_size(RenderContext* ctx, float size) {
    if (sources_.empty()) return false;
    size = clamp_size(size);
    if (has_size(size)) {
        // 档位已存在，但整形参数变过（例如切换了超采样倍率）就得按新参数重烘
        if (!g_shape_dirty) return false;
        g_shape_dirty = false;
        return repack(ctx, sizes_);
    }

    std::vector<float> next = sizes_;
    next.push_back(size);
    // 第 0 档永远保留——单参 get_glyph 用它。
    prune_buckets(next, kMaxBuckets);
    return repack(ctx, next);
}

bool FontAtlas::ensure_sizes(RenderContext* ctx, const std::vector<float>& sizes) {
    if (sources_.empty() || sizes.empty()) return false;

    std::vector<float> next = sizes_;
    bool added = false;
    for (float raw : sizes) {
        const float s = clamp_size(raw);
        bool dup = false;
        for (float e : next) {
            if (std::fabs(e - s) <= kSizeTolerance * std::max(e, 1.0f)) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        next.push_back(s);
        added = true;
    }
    if (!added) {
        if (!g_shape_dirty) return false;
        g_shape_dirty = false;
        return repack(ctx, sizes_);
    }
    g_shape_dirty = false;

    // 第 0 档保持原位，其余按尺寸升序，便于 bucket_for 与日志阅读。
    if (next.size() > 2) std::sort(next.begin() + 1, next.end());
    prune_buckets(next, kMaxBuckets);
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
        float err = std::fabs(font_size - buckets_[i].size) / std::max(buckets_[i].size, 1.0f);
        // 同等误差下优先选**比目标小**的档：放大是双线性插值，笔画轮廓只是被
        // 拉宽一点点；缩小是欠采样，会直接丢掉笔画细节（细竖线断成灰点）。
        // 13px 在 {12, 14} 之间时选 12（放大 8%）而不是 14（缩小 7%）。
        if (buckets_[i].size > font_size) err *= 1.15f;
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

float FontAtlas::cap_height(int bucket) const {
    if (bucket < 0 || bucket >= static_cast<int>(buckets_.size())) return 0.0f;
    const Bucket& b = buckets_[static_cast<std::size_t>(bucket)];
    return b.cap_height > 0.0f ? b.cap_height : b.size * 0.72f;
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
