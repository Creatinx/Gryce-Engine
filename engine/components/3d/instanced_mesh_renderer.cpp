#include "components/3d/instanced_mesh_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "assets/asset_manager.h"
#include "render/material.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

// 确定性哈希（Wang hash 变体）：不依赖 std 库随机数实现，同一 seed 在任何
// 平台/编译器上生成完全一致的实例布局，场景重载后植被不会"跳位"。
uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 取低 24 位映射到 [0,1)
float hash_unit(uint32_t h) {
    return static_cast<float>(h & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

} // namespace

InstancedMeshRenderer::~InstancedMeshRenderer() {
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

void InstancedMeshRenderer::serialize(nlohmann::json& out) const {
    out["mesh_path"] = mesh_path;
    out["material_path"] = material_path;
    if (!inline_material.is_null()) out["material"] = inline_material;
    out["instance_count"] = instance_count;
    out["spacing"] = spacing;
    out["seed"] = seed;
}

void InstancedMeshRenderer::deserialize(const nlohmann::json& in) {
    mesh_path = in.value("mesh_path", "");
    material_path = in.value("material_path", "");
    inline_material = in.contains("material") ? in["material"] : nlohmann::json();
    instance_count = in.value("instance_count", 1);
    spacing = in.value("spacing", 1.0f);
    seed = in.value("seed", 0);

    // 参数变化后交给下一帧 prepare_gpu 重建
    has_cache_ = false;
    geometry_path_loaded_.clear();
    material_path_loaded_.clear();
}

void InstancedMeshRenderer::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
}

void InstancedMeshRenderer::build_instance_matrices(const math::Matrix4f& world) {
    const uint32_t count = instance_count > 0 ? static_cast<uint32_t>(instance_count) : 0u;
    instance_count_ = count;
    if (count == 0) return;

    if (!scratch_[scratch_index_]) {
        scratch_[scratch_index_] = std::make_shared<std::vector<math::Matrix4f>>();
    }
    if (!sphere_scratch_[scratch_index_]) {
        sphere_scratch_[scratch_index_] = std::make_shared<std::vector<math::Vector4f>>();
    }
    auto& buffer = *scratch_[scratch_index_];
    auto& spheres = *sphere_scratch_[scratch_index_];
    if (buffer.size() < count) buffer.resize(count);
    if (spheres.size() < count) spheres.resize(count);

    // 原型网格的本地包围球（GPU 剔除用）。取不到边界时半径给极大值，
    // 保证"必定可见"——宁可多画也不漏画。
    float local_radius = 1.0e30f;
    if (geometry_ && !geometry_->vertices.empty()) {
        double max_d2 = 0.0;
        for (const auto& v : geometry_->vertices) {
            const double d2 = static_cast<double>(v.position.x) * v.position.x +
                              static_cast<double>(v.position.y) * v.position.y +
                              static_cast<double>(v.position.z) * v.position.z;
            if (d2 > max_d2) max_d2 = d2;
        }
        local_radius = static_cast<float>(std::sqrt(max_d2)) + 1.0e-3f;
    }
    // 实例只做绕 Y 旋转 + 均匀缩放（见下方 local 矩阵），因此缩放分量可直接
    // 从矩阵的列长度读出，世界变换再乘一次。Matrix4f 是列主序：第 c 列由
    // (0,c)(1,c)(2,c) 三个元素组成（translation 在第 3 列）。
    const float world_max_scale = std::sqrt(std::max(
        std::max(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) + world(2, 0) * world(2, 0),
                 world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) + world(2, 1) * world(2, 1)),
        std::max(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) + world(2, 2) * world(2, 2),
                 0.0f)));

    // 网格排布：列数取 sqrt(count) 上取整，行数按剩余补齐，整体以原点为中心
    const float cell = std::max(0.01f, spacing);
    const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const uint32_t safe_cols = cols > 0 ? cols : 1u;
    const uint32_t rows = (count + safe_cols - 1) / safe_cols;
    const float half_w = (static_cast<float>(safe_cols) - 1.0f) * cell * 0.5f;
    const float half_d = (static_cast<float>(rows) - 1.0f) * cell * 0.5f;

    const uint32_t base = static_cast<uint32_t>(seed) * 0x9e3779b9u;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t h0 = hash32(base + i * 0x85ebca6bu + 0x165667b1u);
        const uint32_t h1 = hash32(h0 ^ 0x27d4eb2du);
        const uint32_t h2 = hash32(h1 ^ 0x165667b1u);
        const uint32_t h3 = hash32(h2 ^ 0x9e3779b9u);

        const uint32_t col = i % safe_cols;
        const uint32_t row = i / safe_cols;
        const float ox = static_cast<float>(col) * cell - half_w;
        const float oz = static_cast<float>(row) * cell - half_d;
        // 格点抖动：保持网格可读性的同时打散规则感
        const float jx = (hash_unit(h0) - 0.5f) * cell * 0.7f;
        const float jz = (hash_unit(h1) - 0.5f) * cell * 0.7f;

        const float angle = hash_unit(h2) * 2.0f * k_pi;
        const float s = 0.75f + hash_unit(h3) * 0.5f;

        const math::Matrix4f local =
            math::Matrix4f::translate(ox + jx, 0.0f, oz + jz) *
            math::Matrix4f::rotate(angle, math::Vector3f(0.0f, 1.0f, 0.0f)) *
            math::Matrix4f::scale(s, s, s);
        buffer[i] = world * local;

        // 世界包围球：中心取平移列，半径 = 本地半径 × 实例缩放 × 世界缩放。
        // 旋转不影响球。取本地包围球（而不是 AABB）保证剔除保守性。
        if (local_radius < 1.0e29f) {
            const math::Vector3f c(buffer[i](0, 3), buffer[i](1, 3), buffer[i](2, 3));
            spheres[i] = math::Vector4f(c.x, c.y, c.z,
                                        local_radius * s * world_max_scale);
        } else {
            spheres[i] = math::Vector4f(buffer[i](0, 3), buffer[i](1, 3), buffer[i](2, 3),
                                        1.0e30f);
        }
    }
}

void InstancedMeshRenderer::prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& world) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && material_) material_->destroy_gpu(ctx_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        ctx_ = ctx;
    }

    // 几何路径变化：主线程重新加载并展开成平坦顶点流（去索引，见头文件说明）
    if (mesh_path != geometry_path_loaded_) {
        invalidate_gpu();
        geometry_path_loaded_ = mesh_path;
        geometry_.reset();
        flat_vertices_.reset();
        if (!mesh_path.empty()) {
            geometry_ = assets::AssetManager::instance().load_mesh(mesh_path);
        }
        if (!geometry_ || geometry_->empty()) {
            if (!mesh_path.empty()) {
                GLOG_WARN("InstancedMeshRenderer: mesh '{}' unavailable", mesh_path);
            }
            return;
        }

        auto flat = std::make_shared<std::vector<assets::MeshVertex>>();
        if (!geometry_->indices.empty()) {
            // 按索引展开成非索引顶点流：GLMesh::draw() 优先走索引分支
            //（glDrawElements，非实例化），上传索引会让实例化静默失效。
            flat->reserve(geometry_->indices.size());
            for (uint32_t idx : geometry_->indices) {
                if (idx < geometry_->vertices.size()) {
                    flat->push_back(geometry_->vertices[idx]);
                }
            }
        } else {
            *flat = geometry_->vertices;
        }
        flat_vertices_ = std::move(flat);
    }
    if (!flat_vertices_ || flat_vertices_->empty()) return;

    // 材质变化：主线程解析（贴图在渲染线程上传）。
    // material_path 优先；为空则用内联 material 字段；两者都没有时**必须**给一个
    // 默认材质 —— 绘制侧 material_ 为 nullptr 会跳过 mat->bind()，沿用上一个物体
    // 的 uniform，实例就会显示成黑块。
    const std::string material_key = material_path + "\x1f" + inline_material.dump();
    if (material_key != material_path_loaded_) {
        material_path_loaded_ = material_key;
        material_.reset();
        material_uploaded_.store(false, std::memory_order_release);
        if (!material_path.empty()) {
            auto mat = std::make_unique<render::Material>();
            if (!mat->load_from_file(material_path)) {
                GLOG_WARN("InstancedMeshRenderer: material '{}' unavailable, using defaults",
                          material_path);
            }
            material_ = std::move(mat);
        } else if (!inline_material.is_null()) {
            auto mat = std::make_unique<render::Material>();
            mat->deserialize(inline_material);
            material_ = std::move(mat);
        } else {
            material_ = std::make_unique<render::Material>();
        }
    }

    // 实例变换：世界矩阵或实例参数变化才重建（静止物体不产生每帧重建开销）
    const bool params_changed =
        !has_cache_ || last_count_ != instance_count || last_spacing_ != spacing ||
        last_seed_ != seed ||
        std::memcmp(last_world_.m, world.m, sizeof(float) * 16) != 0;
    if (params_changed) {
        build_instance_matrices(world);
        cached_instances_ = scratch_[scratch_index_];
        cached_spheres_ = sphere_scratch_[scratch_index_];
        scratch_index_ = (scratch_index_ + 1) % k_scratch_slots;
        last_world_ = world;
        last_count_ = instance_count;
        last_spacing_ = spacing;
        last_seed_ = seed;
        has_cache_ = true;
    }

    if (instance_count_ == 0 || !cached_instances_) return;

    auto buffer = cached_instances_;
    auto flat = flat_vertices_;
    const uint32_t count = instance_count_;
    const uint32_t bytes = count * static_cast<uint32_t>(sizeof(math::Matrix4f));

    ctx->push_command([this, ctx, buffer, flat, count, bytes,
                       token = alive_token_](render::IRenderBackend*) {
        if (!token->load(std::memory_order_acquire)) return;

        if (!gpu_mesh_handle_.is_valid()) {
            gpu_mesh_handle_ = ctx->create_mesh();
            render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
            if (!mesh) {
                gpu_mesh_handle_ = render::RHIMeshHandle{};
                return;
            }
            // 顶点流：MeshVertex 布局（position/normal/tangent/uv/color，stride 56），
            // 每实例一份原型顶点，实例下标由第二条流推进。
            render::VertexLayout vertex_layout;
            vertex_layout.stride = sizeof(assets::MeshVertex);
            vertex_layout.attributes = {
                {0, render::VertexType::Float3, false, 0},
                {1, render::VertexType::Float3, false, 12},
                {2, render::VertexType::Float3, false, 24},
                {3, render::VertexType::Float2, false, 36},
                {4, render::VertexType::Float3, false, 44}
            };
            mesh->set_layout(vertex_layout);
            mesh->upload_vertices(flat->data(),
                                  static_cast<uint32_t>(flat->size() * sizeof(assets::MeshVertex)),
                                  static_cast<uint32_t>(flat->size()));

            // 实例流：每实例一个 mat4（4 个 vec4，列主序），与 instanced 着色器的
            // attribute location 5..8 一一对应。
            render::VertexLayout instance_layout;
            instance_layout.stride = sizeof(math::Matrix4f);
            instance_layout.attributes = {
                {5, render::VertexType::Float4, false, 0},
                {6, render::VertexType::Float4, false, 16},
                {7, render::VertexType::Float4, false, 32},
                {8, render::VertexType::Float4, false, 48}
            };
            mesh->set_instance_layout(instance_layout);
        }

        render::IMesh* mesh = ctx->mesh(gpu_mesh_handle_);
        if (mesh && bytes <= buffer->size() * sizeof(math::Matrix4f)) {
            mesh->upload_instances(buffer->data(), bytes, count);
        }

        // 材质贴图在渲染线程创建（主线程渲染线程运行期间不做 GPU 上传）
        if (material_ && !material_uploaded_.exchange(true, std::memory_order_acq_rel)) {
            material_->upload_to_gpu(ctx);
        }
    });
}

} // namespace gryce_engine::components