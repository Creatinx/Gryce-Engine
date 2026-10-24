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
// Glyph — 单个字符的字形信息（坐标都是**所属栅格档**下的像素量）
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
// FontAtlas — 字体纹理图集（**多尺寸阶梯**）
//
// 为什么要多档：图集里的字形是"按某个像素尺寸烘出来"的位图。一张图只有一个
// 尺寸时，任何别的绘制尺寸都得靠缩放——13px 的界面文字从一个 32px 的图缩下
// 来，笔画两侧各糊一格灰边（实测明显比 ImGui 的 1:1 文字软），反过来放大则
// 直接是马赛克。所以图集里同一批字形烘若干档（B 档），绘制时选**最近的一档**
// 让缩放比尽量接近 1:1：
//   * 1 倍屏界面文字 → 13 档，1:1，笔画硬朗；
//   * 2 倍屏（HiDPI）→ 26 档，物理像素上仍是 1:1，等于真的重新排版了 2 倍细节；
//   * 引擎自带的 22px 调试文字 → 选 32 档（0.69 缩比）而不是 13 档（1.7 放大）。
//
// 档位按需增长（ensure_size），有 12% 容差 + 上限 4 档：档之间离得太近没有
// 意义，无限加档会把图集撑爆。加档 = 整张图集重烘 + 重传纹理，只在 DPI/缩放
// 真的变了时发生。
// ---------------------------------------------------------------------------
class GRYCE_API FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas();

    // 从单个字体文件加载并生成 atlas（ASCII 可见字符 32-126），初始档 = font_size
    bool init(RenderContext* ctx, const std::string& font_path, float font_size = 32.0f);

    // 从多个字体源加载并合并到同一个 atlas（例如 Latin + CJK 回退），
    // 每个源按其 size 各成一档
    bool init_multi(RenderContext* ctx, const std::vector<FontSource>& sources);

    // 系统字体全部失败时使用：生成一个纯色块 fallback atlas，保证文字至少能以色块形式显示
    bool create_fallback_atlas(RenderContext* ctx, float font_size = 32.0f);

    // -----------------------------------------------------------------------
    // 尺寸阶梯
    // -----------------------------------------------------------------------
    // 确保图集里有 size 这一档（离已有档 12% 以内视为已有）。返回 true 表示
    // 真的重烘了（纹理已换新）。**会 create/destroy 纹理**，运行期必须在 GPU
    // 上下文里调用（RenderContext::run_with_gpu_context_sync）。
    bool ensure_size(RenderContext* ctx, float size);

    // 已有档位里离 font_size 最近的一档（返回下标，恒有效；没有档时返回 -1）
    int   bucket_for(float font_size) const;
    float bucket_size(int bucket) const;
    int   bucket_count() const { return static_cast<int>(buckets_.size()); }
    bool  has_size(float size, float tolerance = 0.12f) const;

    // 获取字符字形（nullptr 表示字符不存在）。单参版本用第 0 档（初始尺寸）。
    const Glyph* get_glyph(char c) const;
    const Glyph* get_glyph(uint32_t codepoint) const;
    const Glyph* get_glyph(int bucket, uint32_t codepoint) const;

    // 垂直翻转所有 glyph 的 UV（用于 Vulkan：纹理按 top-down 解释，需把 OpenGL bottom-up UV 翻回来）
    void flip_uv_vertical();

    // 初始档尺寸（第 0 档）——绘制方应改用 bucket_for/bucket_size 选档
    float font_size() const { return buckets_.empty() ? font_size_ : buckets_[0].size; }
    RHITextureHandle texture_handle() const { return texture_handle_; }
    ITexture* texture() const { return texture_; }
    int atlas_width() const { return atlas_width_; }
    int atlas_height() const { return atlas_height_; }
    const std::unordered_map<uint32_t, Glyph>& glyphs() const { return buckets_.empty() ? empty_glyphs_ : buckets_[0].glyphs; }

    void destroy(RenderContext* ctx);

private:
    struct Bucket {
        float size = 0.0f;
        std::unordered_map<uint32_t, Glyph> glyphs;
    };

    // 真正的实现：按 sources_ × sizes_ 烘一整张图集并上传
    bool repack(RenderContext* ctx, const std::vector<float>& sizes);

    // 只销毁纹理（保留字体源与档位列表，供重烘复用）
    void destroy_texture_only(RenderContext* ctx);

    RHITextureHandle texture_handle_;
    ITexture* texture_ = nullptr;
    std::vector<Bucket> buckets_;
    std::vector<FontSource> sources_;   // 原始字体源（尺寸字段被 sizes_ 取代）
    std::vector<float> sizes_;          // 档位（与 buckets_ 一一对应）
    std::unordered_map<uint32_t, Glyph> empty_glyphs_;
    float font_size_ = 32.0f;
    int atlas_width_ = 0;
    int atlas_height_ = 0;
};

} // namespace gryce_engine::render
