#include "components/3d/lod_group.h"

#include <algorithm>
#include <cmath>

#include "assets/asset_manager.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

LODGroup::LODGroup() {
    material_ = std::make_unique<render::Material>();
}

LODGroup::~LODGroup() {
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

render::Material* LODGroup::ensure_material() {
    if (!material_) material_ = std::make_unique<render::Material>();
    return material_.get();
}

void LODGroup::serialize(nlohmann::json& out) const {
    out["mesh_paths"] = mesh_paths;
    out["screen_size_thresholds"] = screen_size_thresholds;
    out["transition_duration"] = transition_duration;
    out["active_lod"] = active_lod;
}

void LODGroup::deserialize(const nlohmann::json& in) {
    if (in.contains("mesh_paths") && in["mesh_paths"].is_array()) {
        mesh_paths = in["mesh_paths"].get<std::vector<std::string>>();
    }
    if (in.contains("screen_size_thresholds") && in["screen_size_thresholds"].is_array()) {
        screen_size_thresholds = in["screen_size_thresholds"].get<std::vector<float>>();
    }
    transition_duration = in.value("transition_duration", 0.1f);
    active_lod = in.value("active_lod", 0);

    // 场景文件可能整体替换了路径：交给下一次 prepare_gpu 重新加载
    geometry_path_loaded_.clear();
}

int LODGroup::clamped_lod(int lod) const {
    const int n = lod_count();
    if (n <= 0) return 0;
    return std::clamp(lod, 0, n - 1);
}

float LODGroup::threshold_at(int index) const {
    if (index >= 0 && index < static_cast<int>(screen_size_thresholds.size())) {
        return screen_size_thresholds[static_cast<size_t>(index)];
    }
    // 阈值数组缺失时的递推默认值：随 index 递增而递减
    return 0.3f / static_cast<float>(index + 1);
}

int LODGroup::update_lod(float screen_size) {
    const int n = lod_count();
    if (n <= 1) {
        active_lod = 0;
        return active_lod;
    }

    // 无滞回目标：覆盖率低于阈值即下调一级（更远的物体用更低细节）
    int desired = 0;
    for (int k = 0; k + 1 < n; ++k) {
        if (screen_size < threshold_at(k)) {
            desired = k + 1;
        } else {
            break;
        }
    }

    // 滞回带：阈值附近不切换，抑制"边缘反复横跳"。
    // transition_duration 映射为带宽（无逐帧 delta time，无法做时间淡入）。
    const float band = std::clamp(0.05f + transition_duration * 0.5f, 0.05f, 0.5f);
    if (desired != active_lod) {
        if (desired > active_lod) {
            // 降级：需覆盖率确实低于阈值*(1-band)，比升级更严格
            const float th = threshold_at(active_lod);
            if (screen_size > th * (1.0f - band)) desired = active_lod;
        } else {
            // 升级：需覆盖率高于阈值*(1+band)
            const float th = threshold_at(desired);
            if (screen_size < th * (1.0f + band)) desired = active_lod;
        }
    }

    active_lod = desired;
    return active_lod;
}

void LODGroup::invalidate_gpu() {
    if (ctx_ && gpu_mesh_handle_.is_valid()) {
        ctx_->destroy_mesh(gpu_mesh_handle_);
    }
    gpu_mesh_handle_ = render::RHIMeshHandle{};
    // 代次归零：下一帧 prepare_gpu 会重新投递一次上传命令
    queued_generation_.store(0, std::memory_order_release);
}

void LODGroup::world_bounds(const math::Matrix4f& world, math::Vector3f& center,
                            float& radius) const {
    if (!has_bounds_) {
        center = math::Vector3f(world(0, 3), world(1, 3), world(2, 3));
        radius = 0.0f;
        return;
    }
    const math::Vector3f local_center = (local_min_ + local_max_) * 0.5f;
    center = world.transform_point(local_center);

    const math::Vector3f ext = (local_max_ - local_min_) * 0.5f;
    const float local_radius = ext.length();

    // 世界变换的最大轴缩放（旋转不改变包围球半径）
    const float sx = std::sqrt(world(0, 0) * world(0, 0) + world(1, 0) * world(1, 0) +
                               world(2, 0) * world(2, 0));
    const float sy = std::sqrt(world(0, 1) * world(0, 1) + world(1, 1) * world(1, 1) +
                               world(2, 1) * world(2, 1));
    const float sz = std::sqrt(world(0, 2) * world(0, 2) + world(1, 2) * world(1, 2) +
                               world(2, 2) * world(2, 2));
    radius = local_radius * std::max(sx, std::max(sy, sz));
}

void LODGroup::prepare_gpu(render::RenderContext* ctx) {
    if (!ctx) return;

    // 渲染上下文切换（管线重建 / 后端热切换）：旧资源整体重建
    if (ctx_ != ctx) {
        if (ctx_ && gpu_mesh_handle_.is_valid()) ctx_->destroy_mesh(gpu_mesh_handle_);
        if (ctx_ && material_) material_->destroy_gpu(ctx_);
        gpu_mesh_handle_ = render::RHIMeshHandle{};
        queued_generation_.store(0, std::memory_order_release);
        material_uploaded_.store(false, std::memory_order_release);
        ctx_ = ctx;
        geometry_path_loaded_.clear();
    }

    const int lod = clamped_lod(active_lod);
    if (lod != active_lod) active_lod = lod;

    const std::string desired_path =
        (lod >= 0 && lod < lod_count()) ? mesh_paths[static_cast<size_t>(lod)] : std::string();

    // 网格路径变化（LOD 切换 / 路径编辑 / 热重载）：主线程重新加载 MeshData
    if (desired_path != geometry_path_loaded_) {
        invalidate_gpu();
        geometry_path_loaded_ = desired_path;
        geometry_.reset();
        has_bounds_ = false;
        if (!desired_path.empty()) {
            geometry_ = assets::AssetManager::instance().load_mesh(desired_path);
        }
        if (!geometry_ || geometry_->empty()) {
            if (!desired_path.empty()) {
                GLOG_WARN("LODGroup: mesh '{}' unavailable (lod {})", desired_path, lod);
            }
            geometry_.reset();
            return;
        }

        // 由顶点流求局部 AABB（供剔除/屏幕覆盖率使用）
        math::Vector3f lo = geometry_->vertices[0].position;
        math::Vector3f hi = lo;
        for (const auto& v : geometry_->vertices) {
            lo = lo.min(v.position);
            hi = hi.max(v.position);
        }
        local_min_ = lo;
        local_max_ = hi;
        has_bounds_ = true;

        ++geometry_generation_;
    }
    if (!geometry_ || geometry_->empty()) return;

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
                mesh->upload_vertices(
                    geometry->vertices.data(),
                    static_cast<uint32_t>(geometry->vertices.size() * sizeof(assets::MeshVertex)),
                    static_cast<uint32_t>(geometry->vertices.size()));
                mesh->upload_indices(
                    geometry->indices.data(),
                    static_cast<uint32_t>(geometry->indices.size() * sizeof(uint32_t)),
                    static_cast<uint32_t>(geometry->indices.size()));
                GLOG_INFO("LODGroup: uploaded mesh to GPU ({} verts, {} indices, lod {})",
                          geometry->vertices.size(), geometry->indices.size(), active_lod);
            }
        }

        // 材质贴图在渲染线程创建（主线程在渲染线程运行期间不做 GPU 上传）
        if (material_ && !material_uploaded_.exchange(true, std::memory_order_acq_rel)) {
            material_->upload_to_gpu(ctx);
        }
    });
}

} // namespace gryce_engine::components