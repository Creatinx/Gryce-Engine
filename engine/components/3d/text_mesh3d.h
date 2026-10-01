#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/font_atlas.h"
#include "render/render2d.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// TextMesh3D — 3D 世界空间文本（场景里的招牌、世界空间 UI、调试标注）
//
// 组件自带一个 FontAtlas（Renderer2D 的图集不可跨模块共享），在渲染线程内
// 按需创建；文字按字形表展开成 XY 平面的四边形网格（UV 取自图集的 glyph rect，
// 顶点色取自 color），只在 text/font/color/尺寸变化时重建一次，不做每帧上传。
// 因此这里沿用 Terrain 的"代次去重"模式（geometry_generation_ /
// queued_generation_ / need_geometry 守卫），而不是粒子/广告牌的每帧实例流。
// ---------------------------------------------------------------------------
class GRYCE_API TextMesh3D : public Component {
public:
    std::string text;
    std::string font_path;
    float font_size = 1.0f;
    float pixel_height = 0.1f;
    render::Color color = render::Color::white();
    bool double_sided = true;

    TextMesh3D() = default;
    ~TextMesh3D() override;

    const char* type() const override { return "TextMesh3D"; }

    // 字形网格顶点：position(3) + uv(2) + color(4) = 36 字节/顶点。
    struct TextVertex {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // ---- GPU 侧（由 RenderPipeline::collect_text3d 每帧调用一次）----------
    // 参数变化时把字形网格重建并投递上传命令；未变化时直接返回（无每帧上传）。
    // 必须在主线程、render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    // 字形图集纹理（draw_text3d 采样用；句柄仅在渲染线程建好后才有效）
    render::RHITextureHandle texture_handle() const {
        return font_atlas_ ? font_atlas_->texture_handle() : render::RHITextureHandle{};
    }

    // 字形网格的顶点数（> 0 表示本帧有内容可画）
    uint32_t vertex_count() const { return vertex_count_; }

private:
    // 渲染线程内完成：按 path/raster 就绪 FontAtlas（失败退化为内置色块图集）
    bool ensure_font_atlas(render::RenderContext* ctx, const std::string& path, float raster_size);
    // 渲染线程内完成：用图集字形表把 text 展开成 XY 平面的四边形网格
    void build_geometry(std::vector<TextVertex>& vertices, std::vector<uint32_t>& indices,
                        const std::string& text, const render::Color& color,
                        float pixel_height, float raster_size) const;

    std::unique_ptr<render::FontAtlas> font_atlas_;
    // 图集当前对应的来源（只在渲染线程内读写，主线程不碰）
    std::string atlas_path_;
    float atlas_raster_ = 0.0f;

    // 主线程参数快照：任一变化都要重建字形网格
    std::string text_loaded_;
    std::string font_path_loaded_;
    float font_size_loaded_ = 0.0f;
    float pixel_height_loaded_ = 0.0f;
    render::Color color_loaded_ = render::Color(0.0f, 0.0f, 0.0f, 0.0f);

    // 几何重建去重：主线程递增代次，投递时写入原子，命令内用 need_geometry 守卫
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    uint32_t vertex_count_ = 0;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components