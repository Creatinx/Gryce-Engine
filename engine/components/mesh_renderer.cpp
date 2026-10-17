// mesh_renderer.cpp — MeshRenderer 组件实现：LOD 级数选择（距离阈值 / 滞回 /
// 环境变量覆盖）、GPU 顶点布局与网格上传请求、材质参数与贴图合并。
#include "mesh_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {
namespace {

// LOD 行为覆盖开关（环境变量，与 GRYCE_NO_MESH_WELD / GRYCE_FORCE_PCSS 同类）：
//   GRYCE_FORCE_LOD=k      强制所有物体用第 k 级（A/B 截图与画质检查）
//   GRYCE_LOD_PIXEL_ERROR  覆盖 lod_pixel_error 默认值（2.0 像素）
int forced_lod_level() {
    static const int value = []() {
        const char* env = std::getenv("GRYCE_FORCE_LOD");
        return env ? std::atoi(env) : -1;
    }();
    return value;
}

float pixel_error_override() {
    static const float value = []() {
        const char* env = std::getenv("GRYCE_LOD_PIXEL_ERROR");
        return env ? static_cast<float>(std::atof(env)) : 0.0f;
    }();
    return value;
}

// GRYCE_LOD_DEBUG=1：每次选择打印一行（距离 / 半径 / 各级阈值 / 结果），
// 用于调 lod_bias 与 lod_pixel_error —— 只打前几帧，避免刷屏。
bool lod_debug_enabled() {
    static const bool value = std::getenv("GRYCE_LOD_DEBUG") != nullptr;
    return value;
}

// 上传用的顶点布局：position(3) + normal(3) + tangent(3) + uv(2) + color(3) = 14 float。
// 与 assets::MeshVertex 的内存布局一致（后者有 static_assert 保证是 14 个 float），
// 但这里显式列出，避免 MeshVertex 加字段时静默改掉 GPU 侧 stride。
struct VertexGPU {
    float x, y, z;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
    float r, g, b;
};

render::VertexLayout gpu_vertex_layout() {
    render::VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, render::VertexType::Float3, false, 0},
        {1, render::VertexType::Float3, false, 3 * sizeof(float)},
        {2, render::VertexType::Float3, false, 6 * sizeof(float)},
        {3, render::VertexType::Float2, false, 9 * sizeof(float)},
        {4, render::VertexType::Float3, false, 11 * sizeof(float)}
    };
    return layout;
}

void upload_geometry(render::IMesh* mesh, const assets::MeshData& data) {
    if (!mesh) return;
    std::vector<VertexGPU> verts;
    verts.reserve(data.vertices.size());
    for (const auto& v : data.vertices) {
        verts.push_back({
            v.position.x, v.position.y, v.position.z,
            v.normal.x, v.normal.y, v.normal.z,
            v.tangent.x, v.tangent.y, v.tangent.z,
            v.uv.x, v.uv.y,
            v.color.x, v.color.y, v.color.z
        });
    }
    mesh->upload_vertices(verts.data(),
                          static_cast<uint32_t>(verts.size() * sizeof(VertexGPU)),
                          static_cast<uint32_t>(verts.size()));
    mesh->upload_indices(data.indices.data(),
                         static_cast<uint32_t>(data.indices.size() * sizeof(uint32_t)),
                         static_cast<uint32_t>(data.indices.size()));
    mesh->set_layout(gpu_vertex_layout());
}

// 世界变换的最大轴缩放（旋转不改变包围球半径，只受缩放影响）
float max_axis_scale(const math::Matrix4f& world) {
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    return std::max(sx, std::max(sy, sz));
}

}  // namespace

MeshRenderer::MeshRenderer() {
    material = std::make_unique<render::Material>();
}

MeshRenderer::MeshRenderer(const std::string& path) : mesh_path(path) {
    material = std::make_unique<render::Material>();
}

MeshRenderer::~MeshRenderer() {
    // 使延迟执行的异步上传命令失效，避免渲染线程访问已析构的 this
    alive_token_->store(false, std::memory_order_release);
    GLOG_INFO("MeshRenderer::~MeshRenderer: destroying '{}', gpu_mesh_handle.index={}", mesh_path, gpu_mesh_handle_.index);
    release_lod_meshes();
    release_chunk_meshes();
    if (material && ctx_) {
        GLOG_INFO("MeshRenderer::~MeshRenderer: destroying material GPU resources");
        material->destroy_gpu(ctx_);
    }
    if (gpu_mesh_handle_.is_valid() && ctx_) {
        GLOG_INFO("MeshRenderer::~MeshRenderer: destroying gpu_mesh");
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
    GLOG_INFO("MeshRenderer::~MeshRenderer: done '{}'", mesh_path);
}

render::Material* MeshRenderer::ensure_material() {
    if (!material) {
        material = std::make_unique<render::Material>();
    }
    return material.get();
}

void MeshRenderer::release_lod_meshes() {
    const uint32_t mask = lod_uploaded_mask_.exchange(0, std::memory_order_acq_rel);
    lod_pending_mask_.store(0, std::memory_order_release);
    // 级别 0 的句柄由 gpu_mesh_handle_ 持有，不在这里销毁（避免重复销毁同一句柄）
    for (int k = 1; k < assets::k_max_lod_levels; ++k) {
        if ((mask & (1u << k)) && ctx_ && lod_meshes_[k].is_valid()) {
            ctx_->destroy_mesh(lod_meshes_[k]);
        }
        lod_meshes_[k] = render::RHIMeshHandle{};
    }
}

void MeshRenderer::invalidate_gpu_mesh() {
    if (gpu_mesh_handle_.is_valid() && ctx_) {
        GLOG_INFO("MeshRenderer: invalidating gpu_mesh for '{}'", mesh_path);
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
    uploaded_.store(false, std::memory_order_release);
    // LOD：已上传的低模一并丢弃，链本身也作废（源文件可能已变）
    release_lod_meshes();
    release_chunk_meshes();
    chunked_.reset();
    chunks_.reset();
    chunked_resolved_ = false;
    lod_chain_.reset();
    lod_chain_resolved_ = false;
    active_lod_ = 0;
    // 材质贴图一并销毁：下一帧 re-upload 时从 AssetManager 拉取最新数据重建
    if (material && ctx_) {
        material->destroy_gpu(ctx_);
    }
}

void MeshRenderer::serialize(nlohmann::json& out) const {
    out["mesh_path"] = mesh_path;
    out["submesh_index"] = submesh_index;
    out["billboard"] = billboard;
    out["depth_test"] = depth_test;
    if (!lod_enabled) out["lod_enabled"] = false;
    if (lod_bias != 1.0f) out["lod_bias"] = lod_bias;
    if (lod_pixel_error != 2.0f) out["lod_pixel_error"] = lod_pixel_error;
    if (!lod_distances.empty()) out["lod_distances"] = lod_distances;
    if (material) {
        nlohmann::json mat_json;
        material->serialize(mat_json);
        out["material"] = std::move(mat_json);
    }
}

void MeshRenderer::deserialize(const nlohmann::json& in) {
    mesh_path = in.value("mesh_path", "");
    submesh_index = in.value("submesh_index", -1);
    billboard = in.value("billboard", false);
    depth_test = in.value("depth_test", true);
    lod_enabled = in.value("lod_enabled", true);
    lod_bias = in.value("lod_bias", 1.0f);
    lod_pixel_error = in.value("lod_pixel_error", 2.0f);
    if (in.contains("lod_distances") && in["lod_distances"].is_array()) {
        lod_distances = in["lod_distances"].get<std::vector<float>>();
    } else {
        lod_distances.clear();
    }
    ensure_material();
    auto it = in.find("material");
    if (it != in.end()) {
        material->deserialize(*it);
    }
}

render::IMesh* MeshRenderer::upload_to_gpu(render::RenderContext* ctx, const assets::MeshData* data,
                                             bool allow_while_running) {
    if (!ctx || !data || data->empty()) return nullptr;
    if (uploaded_.load(std::memory_order_acquire) && ctx_ == ctx) return gpu_mesh();

    // 渲染线程已接管 GL context 后，主线程不能再做 GL 上传；
    // 热重载等场景会由调用方先 pause_render_thread() 再上传。
    // allow_while_running=true 表示调用方已确保在渲染线程/命令队列中执行。
    if (!allow_while_running && ctx->is_running()) {
        return nullptr;
    }

    if (ctx_ && ctx_ != ctx) {
        // 渲染上下文切换（后端热切换 / 管线重建）：低模属于旧 ctx，须一并释放
        release_lod_meshes();
    }
    ctx_ = ctx;

    // 先完成所有 GPU 资源创建和上传（包括材质），最后再发布 gpu_mesh_，
    // 避免主线程在材质纹理尚未就绪时就开始渲染。
    render::RHIMeshHandle mesh = ctx->create_mesh();
    render::IMesh* mesh_ptr = ctx->mesh(mesh);
    if (!mesh.is_valid() || !mesh_ptr) return nullptr;

    upload_geometry(mesh_ptr, *data);

    // 模型文件自带材质（MTL / assimp）：只补齐组件中**未被显式指定**的字段，
    // 场景文件或代码显式设置的材质属性优先（判定依据是 *_overridden 标志，
    // 不是"值等于默认值" —— 后者无法把 metallic 显式设为 0）。
    if (material && data->material.valid) {
        const assets::MeshMaterialData& mm = data->material;
        const math::Vector3f zero = math::Vector3f::zero();
        if (!material->albedo_overridden) material->albedo_color = mm.albedo_color;
        if (material->emissive_color == zero) material->emissive_color = mm.emissive_color;
        if (material->opacity == 1.0f && mm.opacity < 1.0f) {
            material->opacity = mm.opacity;
            material->blend_mode = render::Material::BlendMode::Blend;
        }
        if (!material->roughness_overridden) material->roughness = mm.roughness;
        if (!material->metallic_overridden) material->metallic = mm.metallic;
        // 双面由模型文件声明（布料/树叶卡片/薄片）：组件没有"显式覆盖"语义，
        // 文件说双面就双面；需要单面时在场景里手动改材质参数即可。
        if (mm.two_sided) material->two_sided = true;
        if (material->albedo_map_path.empty()) material->albedo_map_path = mm.albedo_map;
        if (material->normal_map_path.empty()) material->normal_map_path = mm.normal_map;
        if (material->emissive_map_path.empty()) material->emissive_map_path = mm.emissive_map;
        if (material->roughness_map_path.empty()) material->roughness_map_path = mm.roughness_map;
        if (material->metallic_map_path.empty()) material->metallic_map_path = mm.metallic_map;
        if (material->ao_map_path.empty()) material->ao_map_path = mm.ao_map;
    }

    // 同时上传材质贴图
    if (material) {
        material->upload_to_gpu(ctx);
    }

    gpu_mesh_handle_ = mesh;
    lod_meshes_[0] = mesh;
    lod_uploaded_mask_.fetch_or(1u, std::memory_order_release);
    uploaded_.store(true, std::memory_order_release);

    GLOG_INFO("MeshRenderer: uploaded '{}' (submesh {}) to GPU ({} vertices, {} indices)",
              mesh_path, submesh_index, data->vertices.size(), data->indices.size());
    return mesh_ptr;
}

// ---------------------------------------------------------------------------
// 自动 LOD
// ---------------------------------------------------------------------------
render::RHIMeshHandle MeshRenderer::draw_mesh_handle() const {
    const uint32_t mask = lod_uploaded_mask_.load(std::memory_order_acquire);
    if (mask != 0) {
        // 目标级别可能还在上传队列里：退一级用当前最高分辨率的已就绪网格，
        // 避免"刚切低模的那两帧什么都不画"（比画错级别难看得多）。
        for (int k = std::min(active_lod_, assets::k_max_lod_levels - 1); k >= 0; --k) {
            if (mask & (1u << k)) return lod_meshes_[k];
        }
    }
    return gpu_mesh_handle_;
}

// ---------------------------------------------------------------------------
// 空间分块：提交给渲染管线的绘制接口
// ---------------------------------------------------------------------------
bool MeshRenderer::chunks_ready() const {
    if (!chunked_ || !chunks_ || chunks_->empty()) return false;
    if (chunks_->size() != chunked_->chunks.size()) return false;
    // 判据是"每块至少有一级可用"，不是"每块的目标级别已就绪"。
    // 后者会让某块切级的那 1~2 帧整体退回整模绘制（相机一动就闪一下 L0），
    // 而逐块向下回退（见 draw_mesh_handle_at）画的是同一片几何、只是级别粗一点，
    // 既不会重复绘制也不会缺几何。
    for (const auto& st : *chunks_) {
        if (!st) return false;
        if (st->uploaded.load(std::memory_order_acquire) == 0) return false;
    }
    return true;
}

int MeshRenderer::draw_mesh_count() const {
    // 任何一块一级都没有就整模画一次：一半分块 + 整模混画会把同一片几何画两遍。
    if (!chunked_draw_ || !chunks_ready()) return 1;
    return static_cast<int>(chunks_->size());
}

render::RHIMeshHandle MeshRenderer::draw_mesh_handle_at(int i) const {
    if (!chunked_draw_ || !chunks_ready() || i < 0 ||
        static_cast<std::size_t>(i) >= chunks_->size()) {
        return draw_mesh_handle();
    }
    const ChunkState& st = *(*chunks_)[static_cast<std::size_t>(i)];
    const uint32_t mask = st.uploaded.load(std::memory_order_acquire);
    // 目标级别可能还在上传：退到"当前已就绪的最高级别"，避免那两帧画错级别或破洞
    for (int k = std::clamp(st.active, 0, assets::k_max_lod_levels - 1); k >= 0; --k) {
        if (mask & (1u << k)) return st.meshes[k];
    }
    return draw_mesh_handle();
}

void MeshRenderer::draw_mesh_local_bounds(int i, math::Vector3f& local_center,
                                          float& local_radius) const {
    if (chunked_ && i >= 0 && static_cast<std::size_t>(i) < chunked_->chunks.size()) {
        const assets::LodChunk& c = chunked_->chunks[static_cast<std::size_t>(i)];
        local_center = (c.local_min + c.local_max) * 0.5f;
        local_radius = c.local_radius;
        return;
    }
    if (lod_chain_) {
        local_center = (lod_chain_->local_min + lod_chain_->local_max) * 0.5f;
        local_radius = lod_chain_->local_radius;
        return;
    }
    local_center = math::Vector3f::zero();
    local_radius = 0.0f;
}

void MeshRenderer::world_lod_bounds(const math::Matrix4f& world, math::Vector3f& center,
                                    float& radius) const {
    if (!lod_chain_) {
        center = math::Vector3f(world(0, 3), world(1, 3), world(2, 3));
        radius = 0.0f;
        return;
    }
    const math::Vector3f local_center = (lod_chain_->local_min + lod_chain_->local_max) * 0.5f;
    center = world.transform_point(local_center);
    radius = lod_chain_->local_radius * max_axis_scale(world);
}

void MeshRenderer::chunk_world_bounds(int i, const math::Matrix4f& world,
                                      math::Vector3f& center, float& radius) const {
    math::Vector3f local_center;
    float local_radius = 0.0f;
    draw_mesh_local_bounds(i, local_center, local_radius);
    if (!(local_radius > 0.0f)) {
        // 没有包围信息（链还没生成）：退回实体原点，半径 0 —— 保守但不误剔除
        center = math::Vector3f(world(0, 3), world(1, 3), world(2, 3));
        radius = 0.0f;
        return;
    }
    center = world.transform_point(local_center);
    radius = local_radius * max_axis_scale(world);
}

// 单个包围球的选级（逐物体 / 逐块共用）。
//   world_center / world_radius: 世界空间包围球
//   error_scale: 局部误差 → 世界误差的缩放（世界变换的最大轴缩放）
//   current_level: 上一帧的级别，用于滞回
int MeshRenderer::select_level_for(const assets::LodChain& chain,
                                   const math::Vector3f& world_center, float world_radius,
                                   float error_scale, const math::Vector3f& cam_pos,
                                   float tan_half_fov, int viewport_height,
                                   int current_level) const {
    const int n = chain.level_count();
    if (n <= 1) return 0;

    // 到包围球**最近点**的距离（不是到中心的距离）：教堂这类 50u 级别的大物体
    // 贴近拍摄时，两者差一个半径，按中心算会把近景也判成远景。
    const float center_dist = (world_center - cam_pos).length();
    // 相机进入包围球内部（整片城市/地形这种"把相机包在里面"的巨模型）时，
    // 最近点距离恒为 0 —— 于是 LOD 永久失效、lod_pixel_error 怎么调都没反应。
    // 这里给一个下限"到球心距离的一半"：相机越靠近球心（真的站在模型里），
    // 下限越小、越保守（保持高模）；离球心越远（只是被包围盒包住，实际看的
    // 是远处那片表面），下限越大，允许按真实距离降级。
    const float dist = std::max(std::max(center_dist - world_radius, center_dist * 0.5f), 1e-3f);

    const float pixel_budget =
        pixel_error_override() > 0.0f ? pixel_error_override() : lod_pixel_error;

    // 第 k 级的切换距离：几何误差放大到屏幕上正好占 lod_pixel_error 像素时，
    // 再近就该用更细的一级。lod_distances 非空时用显式世界单位阈值。
    const auto switch_distance = [&](int level) -> float {
        float d = 0.0f;
        if (!lod_distances.empty()) {
            const std::size_t idx = std::min(lod_distances.size() - 1,
                                             static_cast<std::size_t>(std::max(level - 1, 0)));
            d = lod_distances[idx];
        } else {
            d = assets::lod_switch_distance(chain.levels[level].cumulative_error * error_scale,
                                            tan_half_fov, viewport_height, pixel_budget);
        }
        return d * std::max(lod_bias, 0.01f);
    };

    // 强制级别（GRYCE_FORCE_LOD）：画质检查 / LOD 收益对照时跳过距离与滞回，
    // 直接指定级数。强制时跳过滞回 —— 滞回抑制的是边界抖动，强制本就不是边界行为。
    if (const int forced = forced_lod_level(); forced >= 0) {
        return std::clamp(forced, 0, n - 1);
    }

    int desired = 0;
    for (int k = 1; k < n; ++k) {
        const float th = switch_distance(k);
        if (th > 0.0f && dist >= th) desired = k;
    }

    // 滞回带：阈值上下 8% 不切换，抑制"相机在边界上轻微移动就反复换模"的闪烁。
    constexpr float k_band = 0.08f;
    const int cur = std::clamp(current_level, 0, n - 1);
    if (desired > cur) {
        const float th = switch_distance(desired);
        if (th > 0.0f && dist < th * (1.0f + k_band)) desired = cur;
    } else if (desired < cur) {
        const float th = switch_distance(cur);
        if (th > 0.0f && dist > th * (1.0f - k_band)) desired = cur;
    }
    return std::clamp(desired, 0, n - 1);
}

void MeshRenderer::update_lod(render::RenderContext* ctx, const math::Matrix4f& world,
                              const math::Vector3f& cam_pos, float tan_half_fov,
                              int viewport_height) {
    if (!lod_enabled || mesh_path.empty()) {
        active_lod_ = 0;
        return;
    }
    if (ctx && ctx_ && ctx_ != ctx) {
        release_lod_meshes();      // 低模属于旧上下文
        release_chunk_meshes();
    }

    // 首次使用：向 AssetManager 申请 LOD 链（纯 CPU，结果按模型缓存）。
    // 失败也记一笔，避免每帧重试。
    if (!lod_chain_resolved_) {
        lod_chain_resolved_ = true;
        lod_chain_ = assets::AssetManager::instance().load_mesh_lods(mesh_path, submesh_index);
    }
    if (!chunked_resolved_) {
        chunked_resolved_ = true;
        chunked_ = assets::AssetManager::instance().load_mesh_chunk_lods(mesh_path, submesh_index);
        // 每一块都必须有一条可绘制的链：分块绘制是逐块画的，缺一块就是缺一块几何。
        // 构建侧已保证小块退化成只有 L0 的链，这里只是兜底（老缓存/异常路径）。
        if (chunked_ && chunked_->valid()) {
            for (const auto& c : chunked_->chunks) {
                if (!c.chain || c.chain->levels.empty()) {
                    GLOG_WARN("MeshRenderer: chunked LOD '{}' sub{} has a chunk without chain, "
                              "falling back to per-object LOD", mesh_path, submesh_index);
                    chunked_.reset();
                    break;
                }
            }
        }
        if (chunked_ && chunked_->valid()) {
            auto states = std::make_shared<std::vector<std::unique_ptr<ChunkState>>>();
            states->reserve(chunked_->chunks.size());
            for (std::size_t i = 0; i < chunked_->chunks.size(); ++i) {
                states->push_back(std::make_unique<ChunkState>());
            }
            chunks_ = std::move(states);
            chunk_generation_.fetch_add(1, std::memory_order_acq_rel);
        } else {
            chunked_.reset();
            chunks_.reset();
        }
    }

    const bool object_chain_ok = lod_chain_ && lod_chain_->level_count() > 1;
    const bool chunked_ok = chunked_ && chunks_ && !chunks_->empty();
    if (!object_chain_ok && !chunked_ok) {
        active_lod_ = 0;
        return;
    }

    const float scale = max_axis_scale(world);
    const float tan_half = std::max(tan_half_fov, 1e-4f);

    // ---- 先算整模（逐物体）级别：分块要不要接管，得拿它当基准 ----
    std::size_t whole_tris = 0;
    if (object_chain_ok) {
        math::Vector3f center;
        float radius = 0.0f;
        world_lod_bounds(world, center, radius);
        whole_lod_ = select_level_for(*lod_chain_, center, radius, scale, cam_pos, tan_half,
                                      viewport_height, whole_lod_);
        whole_tris = lod_chain_->levels[std::clamp(whole_lod_, 0, lod_chain_->level_count() - 1)]
                         .triangles;
    } else if (chunked_ok) {
        whole_tris = chunked_->base_triangles;
    }

    // ---- 分块：每块按自己的距离选级，并统计"逐块加起来的三角形数" ----
    // 关键判据：分块只有真的省三角形才接管。远看时整模早就能降到 L2/L3，
    // 分成 8 块反而要按块各降一级、总数更多，还要多付 24 次绘制与每块 L0 的显存；
    // 近看（相机贴着/站在模型里）时整模判据只能选 L0，分块才能按远近有取有舍。
    bool use_chunks = false;
    int chunk_max_level = 0;
    if (chunked_ok) {
        std::size_t chunk_tris = 0;
        int histogram[assets::k_max_lod_levels] = {};
        for (std::size_t i = 0; i < chunks_->size(); ++i) {
            const assets::LodChunk& c = chunked_->chunks[i];
            const math::Vector3f local_center = (c.local_min + c.local_max) * 0.5f;
            const math::Vector3f world_center = world.transform_point(local_center);
            ChunkState& st = *(*chunks_)[i];
            st.active = select_level_for(*c.chain, world_center, c.local_radius * scale, scale,
                                         cam_pos, tan_half, viewport_height, st.active);
            const int lv = std::clamp(st.active, 0, c.chain->level_count() - 1);
            chunk_tris += c.chain->levels[lv].triangles;
            histogram[st.active] += 1;
            chunk_max_level = std::max(chunk_max_level, st.active);
        }
        // 2% 余量避免在边界来回抖（级别选择本身也有 8% 滞回带）
        use_chunks = whole_tris == 0 || chunk_tris + chunk_tris / 50 < whole_tris;

        if (use_chunks) {
            for (std::size_t i = 0; i < chunks_->size(); ++i) {
                // 连 L0 一起投递：整模句柄在分块路径下不参与绘制，每块的 L0 得自己上传。
                request_chunk_upload(ctx, i, (*chunks_)[i]->active);
            }
        }

        if (lod_debug_enabled()) {
            static int logged = 0;
            if (logged < 24 && mesh_path.find("fbx") != std::string::npos) {
                ++logged;
                std::string hist;
                for (int k = 0; k < assets::k_max_lod_levels; ++k) {
                    if (histogram[k] > 0) {
                        hist += " L" + std::to_string(k) + "x" + std::to_string(histogram[k]);
                    }
                }
                math::Vector3f obj_center;
                float obj_radius = 0.0f;
                world_lod_bounds(world, obj_center, obj_radius);
                GLOG_INFO("MeshLOD chunk: '{}' sub{} {} chunks (model center {:.1f}u away) ->{} | "
                          "tris whole L{}({}) vs chunked({}) => {} ; ready={} uploads={}",
                          mesh_path, submesh_index, chunks_->size(),
                          (obj_center - cam_pos).length(), hist, whole_lod_, whole_tris, chunk_tris,
                          use_chunks ? "chunked" : "whole", chunks_ready() ? 1 : 0,
                          chunk_upload_ok_.load(std::memory_order_relaxed));
            }
        }
    }

    if (use_chunks) {
        chunked_draw_ = true;
        active_lod_ = chunk_max_level;
        return;
    }

    // ---- 整模路径（分块没赢，或该模型没分块）----
    chunked_draw_ = false;
    if (!object_chain_ok) {
        active_lod_ = 0;
        return;
    }
    active_lod_ = whole_lod_;
    if (whole_lod_ > 0) request_lod_upload(ctx, whole_lod_);

    if (lod_debug_enabled()) {
        static int logged = 0;
        if (logged < 12 && mesh_path.find("fbx") != std::string::npos) {
            ++logged;
            math::Vector3f center;
            float radius = 0.0f;
            world_lod_bounds(world, center, radius);
            GLOG_INFO("MeshLOD select: '{}' sub{} center {:.1f}u away, radius {:.2f}, "
                      "scale {:.3f} -> L{}",
                      mesh_path, submesh_index, (center - cam_pos).length(), radius, scale,
                      active_lod_);
        }
    }
}

void MeshRenderer::request_lod_upload(render::RenderContext* ctx, int level) {
    if (!ctx || !lod_chain_ || level <= 0 || level >= assets::k_max_lod_levels) return;
    if (level >= lod_chain_->level_count()) return;

    const uint32_t bit = 1u << level;
    if (lod_uploaded_mask_.load(std::memory_order_acquire) & bit) return;
    // 已投递（命令在途）：不重复建 mesh
    if (lod_pending_mask_.fetch_or(bit, std::memory_order_acq_rel) & bit) return;

    auto geometry = lod_chain_->levels[level].mesh;
    ctx->push_command([this, ctx, level, bit, geometry,
                       token = alive_token_](render::IRenderBackend*) {
        const auto clear_pending = [this, bit]() {
            lod_pending_mask_.fetch_and(~bit, std::memory_order_acq_rel);
        };
        if (!token->load(std::memory_order_acquire)) {
            // 组件已析构：命令持有 this 是悬垂的，连清除都不做
            return;
        }
        render::RHIMeshHandle handle = ctx->create_mesh();
        if (!handle.is_valid()) {
            clear_pending();
            return;
        }
        render::IMesh* mesh = ctx->mesh(handle);
        if (!mesh) {
            ctx->destroy_mesh(handle);
            clear_pending();
            return;
        }
        upload_geometry(mesh, *geometry);
        lod_meshes_[level] = handle;
        lod_uploaded_mask_.fetch_or(bit, std::memory_order_release);
        clear_pending();
        GLOG_INFO("MeshRenderer: LOD L{} uploaded for '{}' ({} verts, {} indices)",
                  level, mesh_path, geometry->vertices.size(), geometry->indices.size());
    });
}

// ---------------------------------------------------------------------------
// 空间分块：GPU 资源
// ---------------------------------------------------------------------------
void MeshRenderer::release_chunk_meshes() {
    // 让在途命令作废：它们捕获了 generation，一旦不匹配就自己销毁刚建的 mesh
    chunk_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!chunks_) return;
    for (auto& st : *chunks_) {
        if (!st) continue;
        const uint32_t mask = st->uploaded.exchange(0, std::memory_order_acq_rel);
        st->pending.store(0, std::memory_order_release);
        for (int k = 0; k < assets::k_max_lod_levels; ++k) {
            if ((mask & (1u << k)) && ctx_ && st->meshes[k].is_valid()) {
                ctx_->destroy_mesh(st->meshes[k]);
            }
            st->meshes[k] = render::RHIMeshHandle{};
        }
        st->active = 0;
    }
}

void MeshRenderer::request_chunk_upload(render::RenderContext* ctx, std::size_t chunk, int level) {
    if (!ctx || !chunked_ || !chunks_ || chunk >= chunks_->size()) return;
    const assets::LodChunk& c = chunked_->chunks[chunk];
    // level 0 也走这条路径：分块绘制时整模的 gpu_mesh_handle_ 用不上，每块的 L0
    // 必须自己上传到 st.meshes[0]，否则这块永远"未就绪"、chunks_ready() 恒假。
    if (!c.chain || level < 0 || level >= c.chain->level_count()) return;
    if (level >= assets::k_max_lod_levels) return;

    ChunkState& st = *(*chunks_)[chunk];
    const uint32_t bit = 1u << level;
    if (st.uploaded.load(std::memory_order_acquire) & bit) return;
    if (st.pending.fetch_or(bit, std::memory_order_acq_rel) & bit) return;

    // 捕获 states 快照：invalidate 重建 chunks_ 后，旧命令仍然写进这块已脱管的
    // vector，不会踩空指针；generation 不匹配则把刚建的 mesh 直接丢掉。
    auto geometry = c.chain->levels[level].mesh;
    auto states = chunks_;
    const uint32_t generation = chunk_generation_.load(std::memory_order_acquire);
    ctx->push_command([this, ctx, chunk, level, bit, geometry, states, generation,
                       token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;   // 组件已析构
        render::RHIMeshHandle handle = ctx->create_mesh();
        if (!handle.is_valid()) {
            if (chunk < states->size() && (*states)[chunk]) {
                (*states)[chunk]->pending.fetch_and(~bit, std::memory_order_acq_rel);
            }
            return;
        }
        render::IMesh* mesh = ctx->mesh(handle);
        if (!mesh) {
            ctx->destroy_mesh(handle);
            if (chunk < states->size() && (*states)[chunk]) {
                (*states)[chunk]->pending.fetch_and(~bit, std::memory_order_acq_rel);
            }
            return;
        }
        upload_geometry(mesh, *geometry);

        const bool stale = chunk_generation_.load(std::memory_order_acquire) != generation ||
                           chunk >= states->size() || !(*states)[chunk];
        if (stale) {
            // 这批块已被释放/重建（热重载、上下文切换）：直接回收，不写状态
            ctx->destroy_mesh(handle);
            return;
        }
        ChunkState& target = *(*states)[chunk];
        target.meshes[level] = handle;
        target.uploaded.fetch_or(bit, std::memory_order_release);
        target.pending.fetch_and(~bit, std::memory_order_acq_rel);
        chunk_upload_ok_.fetch_add(1, std::memory_order_relaxed);
    });
}

} // namespace gryce_engine::components
