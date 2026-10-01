#include "components/3d/text_mesh3d.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include "render/mesh.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// 图集栅格化的像素字号下限/上限：font_size 的默认值 1.0 若直接当像素用会退化
// 成 1px 图集，这里钳制到可用范围（同时也让 font_size 只影响清晰度，不影响尺寸）。
constexpr float k_min_raster_size = 8.0f;
constexpr float k_max_raster_size = 256.0f;

// 解析字体文件路径。优先级与 Renderer2D 完全一致：显式 font_path → 项目内置 Roboto
// → 常见系统字体；全部失败返回空，由调用方退回 FontAtlas 的色块图集。
// 注意 res:/ 映射到项目根，引擎仓库根的 third_party 并非资源目录，因此不能靠它找到内置字体。
std::string resolve_font_path(const std::string& requested) {
    if (!requested.empty()) {
        const std::string resolved = resources::ResourcePath::resolve(requested);
        if (!resolved.empty() && std::filesystem::exists(resolved)) return resolved;
    }
    std::string bundled = resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf");
    if (!bundled.empty() && std::filesystem::exists(bundled)) return bundled;
    bundled = resources::Project::instance().root() +
              "/third_party/imgui/misc/fonts/Roboto-Medium.ttf";
    if (std::filesystem::exists(bundled)) return bundled;

    // 内置字体不存在时退回系统字体（候选列表与 Renderer2D 保持一致）
    std::string font_dir = "C:\\Windows\\Fonts\\";
#ifdef _WIN32
    char win_dir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        font_dir = win_dir;
        if (font_dir.back() != '\\') font_dir += '\\';
        font_dir += "Fonts\\";
    }
#endif
    static const char* k_system_fonts[] = {
        "arial.ttf", "segoeui.ttf", "tahoma.ttf", "calibri.ttf",
        "verdana.ttf", "times.ttf", "msyh.ttc", "simhei.ttf"};
    for (const char* name : k_system_fonts) {
        const std::string candidate = font_dir + name;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

} // namespace

TextMesh3D::~TextMesh3D() {
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
    if (font_atlas_) {
        font_atlas_->destroy(ctx_);
        font_atlas_.reset();
    }
}

void TextMesh3D::serialize(nlohmann::json& out) const {
    out["text"] = text;
    out["font_path"] = font_path;
    out["font_size"] = font_size;
    out["pixel_height"] = pixel_height;
    out["color"] = { color.r, color.g, color.b, color.a };
    out["double_sided"] = double_sided;
}

void TextMesh3D::deserialize(const nlohmann::json& in) {
    text = in.value("text", "");
    font_path = in.value("font_path", "");
    font_size = in.value("font_size", 1.0f);
    pixel_height = in.value("pixel_height", 0.1f);
    auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
    if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
    double_sided = in.value("double_sided", true);

    // 参数快照清空：下一帧 prepare_gpu 会重建字形网格
    text_loaded_.clear();
    font_path_loaded_.clear();
}

void TextMesh3D::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    // 代次归零：下一帧 prepare_gpu 会重新投递一次上传命令
    queued_generation_.store(0, std::memory_order_release);
}

bool TextMesh3D::ensure_font_atlas(render::RenderContext* ctx, const std::string& path,
                                   float raster_size) {
    if (font_atlas_ && path == atlas_path_ &&
        std::abs(raster_size - atlas_raster_) < 0.01f && font_atlas_->texture()) {
        return true;
    }

    if (!font_atlas_) font_atlas_ = std::make_unique<render::FontAtlas>();

    bool ok = false;
    const std::string resolved = resolve_font_path(path);
    if (!resolved.empty()) {
        ok = font_atlas_->init(ctx, resolved, raster_size);
    }
    if (!ok) {
        GLOG_WARN("TextMesh3D: font '{}' unavailable, using built-in fallback glyph atlas", path);
        ok = font_atlas_->create_fallback_atlas(ctx, raster_size);
    }
    if (!ok) {
        GLOG_ERROR("TextMesh3D: failed to create font atlas, 3D text disabled");
        font_atlas_->destroy(ctx);
        font_atlas_.reset();
        atlas_path_.clear();
        atlas_raster_ = 0.0f;
        return false;
    }

    atlas_path_ = path;
    atlas_raster_ = raster_size;
    return true;
}

void TextMesh3D::build_geometry(std::vector<TextVertex>& vertices, std::vector<uint32_t>& indices,
                                const std::string& text, const render::Color& color,
                                float pixel_height, float raster_size) const {
    if (!font_atlas_ || raster_size <= 0.0f) return;

    // 世界单位/像素：像素高度 pixel_height 对应 raster_size 个栅格像素，
    // 于是字符的像素度量可以直接乘 scale 得到世界尺寸。
    const float scale = pixel_height / raster_size;
    // 行距 1.2 倍 em 高，与常见排版一致
    const float line_advance = pixel_height * 1.2f;

    float cursor_x = 0.0f;   // 当前行基线起点的水平推进（像素）
    float cursor_y = 0.0f;   // 向下累加的行偏移（像素），世界 Y 上取负

    uint32_t next_index = 0;
    for (char ch : text) {
        if (ch == '\n') {
            cursor_x = 0.0f;
            cursor_y += line_advance;
            continue;
        }
        if (ch == '\r') continue;

        const render::Glyph* g = font_atlas_->get_glyph(ch);
        if (!g) continue;

        const float base_y = -cursor_y;                       // 世界 Y，+Y 向上
        const float x0 = (cursor_x + g->offset_x) * scale;
        const float x1 = x0 + g->width * scale;
        // offset_y 是字形顶边相对基线的像素偏移（stb_truetype 约定：向上为负）
        const float y_top = base_y - g->offset_y * scale;
        const float y_bot = y_top - g->height * scale;

        // 4 个顶点按 左上→左下→右下→右上 排列，UV 用 top-down 图集坐标
        // （uv0 = 字形左上、uv1 = 字形右下），保证文字正立。
        vertices.push_back(TextVertex{x0, y_top, 0.0f, g->uv0_x, g->uv0_y,
                                      color.r, color.g, color.b, color.a});
        vertices.push_back(TextVertex{x0, y_bot, 0.0f, g->uv0_x, g->uv1_y,
                                      color.r, color.g, color.b, color.a});
        vertices.push_back(TextVertex{x1, y_bot, 0.0f, g->uv1_x, g->uv1_y,
                                      color.r, color.g, color.b, color.a});
        vertices.push_back(TextVertex{x1, y_top, 0.0f, g->uv1_x, g->uv0_y,
                                      color.r, color.g, color.b, color.a});

        // 逆时针绕序（从 +Z 看向 XY 平面时为正面）
        indices.push_back(next_index + 0);
        indices.push_back(next_index + 1);
        indices.push_back(next_index + 2);
        indices.push_back(next_index + 0);
        indices.push_back(next_index + 2);
        indices.push_back(next_index + 3);
        next_index += 4;

        cursor_x += g->advance;
    }
}

void TextMesh3D::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    bool dirty = false;
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        ctx_ = ctx;
        dirty = true;
    }

    // 参数快照比对：只有在 text/font/颜色/尺寸变化时才重建字形网格（不做每帧上传）
    if (text != text_loaded_ || font_path != font_path_loaded_ ||
        font_size != font_size_loaded_ || pixel_height != pixel_height_loaded_ ||
        color.r != color_loaded_.r || color.g != color_loaded_.g ||
        color.b != color_loaded_.b || color.a != color_loaded_.a) {
        dirty = true;
    }
    if (!dirty) return;

    text_loaded_ = text;
    font_path_loaded_ = font_path;
    font_size_loaded_ = font_size;
    pixel_height_loaded_ = pixel_height;
    color_loaded_ = color;

    ++geometry_generation_;

    const uint64_t generation = geometry_generation_;
    const bool need_geometry = generation != queued_generation_.load(std::memory_order_acquire);
    if (!need_geometry) return;
    // 先登记代次再投递：命令在渲染线程执行前，本帧不会再重复投递同一份网格
    queued_generation_.store(generation, std::memory_order_release);

    const std::string text_copy = text;
    const std::string font_path_copy = font_path;
    const render::Color color_copy = color;
    const float pixel_height_copy = pixel_height;
    const float raster_size = std::clamp(font_size, k_min_raster_size, k_max_raster_size);

    ctx->push_command([this, ctx, text_copy, font_path_copy, color_copy, pixel_height_copy,
                       raster_size, token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        // 字形度量来自图集，而图集的 GPU 纹理只能在渲染线程创建，因此整套
        // "确保图集 → 展开字形四边形 → 上传" 都放在这条命令里执行。
        if (!ensure_font_atlas(ctx, font_path_copy, raster_size)) return;

        std::vector<TextVertex> vertices;
        std::vector<uint32_t> indices;
        build_geometry(vertices, indices, text_copy, color_copy, pixel_height_copy, raster_size);
        if (vertices.empty() || indices.empty()) {
            vertex_count_ = 0;
            return;
        }

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 顶点布局：position(3)+uv(2)+color(4)，与 text3d 着色器的
            // attribute location 0..2 一一对应。
            render::VertexLayout layout;
            layout.stride = sizeof(TextVertex);
            layout.attributes = {
                {0, render::VertexType::Float3, false, 0},
                {1, render::VertexType::Float2, false, 12},
                {2, render::VertexType::Float4, false, 20}
            };
            mesh->set_layout(layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh) {
            mesh->upload_vertices(vertices.data(),
                                  static_cast<uint32_t>(vertices.size() * sizeof(TextVertex)),
                                  static_cast<uint32_t>(vertices.size()));
            mesh->upload_indices(indices.data(),
                                 static_cast<uint32_t>(indices.size() * sizeof(uint32_t)),
                                 static_cast<uint32_t>(indices.size()));
        }
        vertex_count_ = static_cast<uint32_t>(vertices.size());
    });
}

} // namespace gryce_engine::components