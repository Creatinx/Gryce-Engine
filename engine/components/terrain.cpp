#include "components/terrain.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "render/material.h"
#include "render/mesh.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

// 简单的哈希噪声 [-1, 1]
float hash_noise(int x, int z, int seed) {
    int n = x * 374761393 + z * 668265263 + seed * 1013904223;
    n = (n ^ (n >> 13)) * 1274126177;
    n = n ^ (n >> 16);
    return static_cast<float>(n & 0x7fffffff) / static_cast<float>(0x7fffffff) * 2.0f - 1.0f;
}

// 平滑插值
float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float value_noise(float x, float z, int seed) {
    int ix = static_cast<int>(std::floor(x));
    int iz = static_cast<int>(std::floor(z));
    float fx = x - static_cast<float>(ix);
    float fz = z - static_cast<float>(iz);

    float v00 = hash_noise(ix, iz, seed);
    float v10 = hash_noise(ix + 1, iz, seed);
    float v01 = hash_noise(ix, iz + 1, seed);
    float v11 = hash_noise(ix + 1, iz + 1, seed);

    float sx = smooth(fx);
    float sz = smooth(fz);

    float v0 = v00 + (v10 - v00) * sx;
    float v1 = v01 + (v11 - v01) * sx;
    return v0 + (v1 - v0) * sz;
}

} // namespace

Terrain::Terrain() {
    material_ = std::make_unique<render::Material>();
}

Terrain::~Terrain() {
    // 使延迟执行的异步上传命令失效，避免渲染线程访问已析构的 this
    alive_token_->store(false, std::memory_order_release);
    if (!ctx_) return;
    if (material_) {
        material_->destroy_gpu(ctx_);
    }
    if (gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
    }
}

render::Material* Terrain::ensure_material() {
    if (!material_) material_ = std::make_unique<render::Material>();
    return material_.get();
}

void Terrain::serialize(nlohmann::json& out) const {
    out["width"] = width;
    out["depth"] = depth;
    out["resolution"] = resolution;
    out["height_scale"] = height_scale;
    out["base_texture_path"] = base_texture_path;
    out["seed"] = seed;
    out["heightmap"] = heightmap_;
}

void Terrain::deserialize(const nlohmann::json& in) {
    width = in.value("width", 100.0f);
    depth = in.value("depth", 100.0f);
    resolution = in.value("resolution", 64);
    height_scale = in.value("height_scale", 10.0f);
    base_texture_path = in.value("base_texture_path", std::string());
    seed = in.value("seed", 0);

    const int expected = heightmap_size();
    if (in.contains("heightmap") && in["heightmap"].is_array()) {
        heightmap_.clear();
        for (const auto& v : in["heightmap"]) {
            heightmap_.push_back(v.get<float>());
        }
    }
    if (static_cast<int>(heightmap_.size()) != expected) {
        ensure_heightmap();
        generate_noise();
    }
    // 高度图可能被场景文件整体替换：交给下一次 prepare_gpu 重建 GPU 网格
    mark_dirty();
}

int Terrain::heightmap_size() const {
    return std::max(2, resolution + 1);
}

float Terrain::height_at(int x, int z) const {
    const int s = heightmap_size();
    x = std::clamp(x, 0, s - 1);
    z = std::clamp(z, 0, s - 1);
    return heightmap_[z * s + x];
}

void Terrain::set_height(int x, int z, float h) {
    const int s = heightmap_size();
    if (x < 0 || x >= s || z < 0 || z >= s) return;
    heightmap_[z * s + x] = std::clamp(h, 0.0f, 1.0f);
    mark_dirty();
}

void Terrain::ensure_heightmap() {
    const int s = heightmap_size();
    heightmap_.assign(s * s, 0.0f);
    mark_dirty();
}

void Terrain::normalize_heights() {
    if (heightmap_.empty()) return;
    float lo = heightmap_[0];
    float hi = heightmap_[0];
    for (float h : heightmap_) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    if (hi - lo < 1e-6f) return;
    const float inv = 1.0f / (hi - lo);
    for (float& h : heightmap_) {
        h = (h - lo) * inv;
    }
    mark_dirty();
}

void Terrain::generate_noise() {
    ensure_heightmap();
    const int s = heightmap_size();
    const float scale = 0.05f;

    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            float fx = static_cast<float>(x) * scale;
            float fz = static_cast<float>(z) * scale;
            float h = value_noise(fx, fz, seed) * 0.5f + 0.5f;
            h += value_noise(fx * 2.0f, fz * 2.0f, seed + 1) * 0.25f;
            h += value_noise(fx * 4.0f, fz * 4.0f, seed + 2) * 0.125f;
            heightmap_[z * s + x] = std::clamp(h, 0.0f, 1.0f);
        }
    }
    normalize_heights();
}

math::Vector3f Terrain::compute_normal(int x, int z) const {
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);

    float hL = height_at(x - 1, z) * height_scale;
    float hR = height_at(x + 1, z) * height_scale;
    float hD = height_at(x, z - 1) * height_scale;
    float hU = height_at(x, z + 1) * height_scale;

    math::Vector3f normal(hL - hR, 2.0f * dx, hD - hU);
    return normal.normalized();
}

assets::MeshData Terrain::build_mesh_data() const {
    assets::MeshData data;
    const int s = heightmap_size();
    const float half_w = width * 0.5f;
    const float half_d = depth * 0.5f;
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);

    data.vertices.reserve(s * s);
    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            assets::MeshVertex v;
            v.position = math::Vector3f(
                -half_w + static_cast<float>(x) * dx,
                height_at(x, z) * height_scale,
                -half_d + static_cast<float>(z) * dz
            );
            v.normal = compute_normal(x, z);
            v.tangent = math::Vector3f::right();
            v.uv = math::Vector2f(
                static_cast<float>(x) / static_cast<float>(resolution),
                static_cast<float>(z) / static_cast<float>(resolution)
            );
            v.color = math::Vector3f::one();
            data.vertices.push_back(v);
        }
    }

    data.indices.reserve(resolution * resolution * 6);
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            uint32_t i0 = z * s + x;
            uint32_t i1 = z * s + (x + 1);
            uint32_t i2 = (z + 1) * s + x;
            uint32_t i3 = (z + 1) * s + (x + 1);

            data.indices.push_back(i0);
            data.indices.push_back(i2);
            data.indices.push_back(i1);

            data.indices.push_back(i1);
            data.indices.push_back(i2);
            data.indices.push_back(i3);
        }
    }

    data.name = "TerrainMesh";
    return data;
}

// ---------------------------------------------------------------------------
// GPU 化
// ---------------------------------------------------------------------------
void Terrain::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    // 代次归零：下一帧 prepare_gpu 会重新投递一次上传命令
    queued_generation_.store(0, std::memory_order_release);
}

void Terrain::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                           float& radius) const {
    // 地形网格在局部空间以原点为中心、Y 从 0 抬到 height_scale
    const float half_h = height_scale * 0.5f;
    center = world.transform_point(math::Vector3f(0.0f, half_h, 0.0f));

    const float half_w = width * 0.5f;
    const float half_d = depth * 0.5f;
    const float local_radius = std::sqrt(half_w * half_w + half_d * half_d + half_h * half_h);

    // 世界变换的最大轴缩放（旋转不改变包围球半径）
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = local_radius * std::max(sx, std::max(sy, sz));
}

void Terrain::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && material_) material_->destroy_gpu(ctx_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        queued_generation_.store(0, std::memory_order_release);
        material_uploaded_.store(false, std::memory_order_release);
        ctx_ = ctx;
        gpu_dirty_ = true;
    }

    // 网格重建：高度图被编辑（gpu_dirty_）或尺寸/分辨率/高度缩放/种子变化。
    // 静止地形每帧只做一次标量比对，不构建 MeshData、不投递命令。
    const bool params_changed = gpu_dirty_ || last_width_ != width || last_depth_ != depth ||
                                last_resolution_ != resolution ||
                                last_height_scale_ != height_scale || last_seed_ != seed;
    if (params_changed) {
        // resolution 变化会改变高度图维度：重建并按新维度重新生成噪声
        if (static_cast<int>(heightmap_.size()) != heightmap_size()) {
            ensure_heightmap();
            generate_noise();
        }
        auto mesh = std::make_shared<assets::MeshData>(build_mesh_data());
        if (mesh->empty()) return;

        geometry_ = std::move(mesh);
        ++geometry_generation_;
        last_width_ = width;
        last_depth_ = depth;
        last_resolution_ = resolution;
        last_height_scale_ = height_scale;
        last_seed_ = seed;
        gpu_dirty_ = false;
    }
    if (!geometry_ || geometry_->empty()) return;

    // 基础纹理路径变化：映射到材质的 albedo 贴图上（贴图仍在渲染线程创建）
    if (base_texture_path != last_texture_path_) {
        last_texture_path_ = base_texture_path;
        ensure_material();
        material_->albedo_map_path = base_texture_path;
        material_->use_albedo_map = !base_texture_path.empty();
        material_uploaded_.store(false, std::memory_order_release);
    }

    const uint64_t generation = geometry_generation_;
    const bool need_geometry = generation != queued_generation_.load(std::memory_order_acquire);
    const bool need_material =
        material_ && !material_uploaded_.load(std::memory_order_acquire);
    if (!need_geometry && !need_material) return;

    auto geometry = geometry_;
    if (need_geometry) {
        // 先登记代次再投递：命令在渲染线程执行前，本帧不会再重复投递同一份网格
        queued_generation_.store(generation, std::memory_order_release);
    }
    ctx->push_command([this, ctx, geometry, need_geometry,
                       token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        // 只有"本代网格尚未投递"的那次命令才上传顶点/索引；仅因材质待上传
        // 而投递的命令不去碰网格（否则渲染线程追上命令队列前会重复上传）。
        if (need_geometry) {
            if (!gpu_mesh_handle_.is_valid()) {
                gpu_mesh_handle_ = ctx->create_mesh();
                render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
                if (!mesh) {
                    gpu_mesh_handle_ = render::RHIMeshHandle{};
                    return;
                }
                // 顶点布局：MeshVertex（position/normal/tangent/uv/color，stride 56），
                // 与 MeshRenderer 上传模型网格时使用的布局一致（location 0..4）。
                render::VertexLayout layout;
                layout.stride = sizeof(assets::MeshVertex);
                layout.attributes = {
                    {0, render::VertexType::Float3, false, 0},
                    {1, render::VertexType::Float3, false, 12},
                    {2, render::VertexType::Float3, false, 24},
                    {3, render::VertexType::Float2, false, 36},
                    {4, render::VertexType::Float3, false, 44}
                };
                mesh->set_layout(layout);
            }

            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (mesh) {
                // 高度图编辑后顶点数通常不变，直接原地重传；分辨率变化时后端会
                // 自动扩容缓冲（GL/Vulkan 的 upload_* 都按 size 判断是否重建）。
                mesh->upload_vertices(
                    geometry->vertices.data(),
                    static_cast<uint32_t>(geometry->vertices.size() * sizeof(assets::MeshVertex)),
                    static_cast<uint32_t>(geometry->vertices.size()));
                mesh->upload_indices(
                    geometry->indices.data(),
                    static_cast<uint32_t>(geometry->indices.size() * sizeof(uint32_t)),
                    static_cast<uint32_t>(geometry->indices.size()));
                GLOG_INFO("Terrain: uploaded mesh to GPU ({} verts, {} indices, {}x{})",
                          geometry->vertices.size(), geometry->indices.size(), resolution,
                          resolution);
            }
        }

        // 材质贴图在渲染线程创建（主线程在渲染线程运行期间不做 GPU 上传）
        if (material_ && !material_uploaded_.exchange(true, std::memory_order_acq_rel)) {
            material_->upload_to_gpu(ctx);
        }
    });
}

} // namespace gryce_engine::components
