#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "components/component.h"
#include "export.h"
#include "assets/mesh_data.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/mesh.h"

namespace gryce_engine::render { class Material; }

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Terrain — 基础地形组件（M2 基础版）
//
// 使用规则高度图网格描述 3D 地形，支持程序化生成和简单参数编辑。
// 高度图数据在 CPU 侧维护，由组件自己构建 MeshData 并投递 GPU 上传命令
// （见 prepare_gpu），渲染管线把它与普通不透明网格同样绘制（PBR 管线）。
// ---------------------------------------------------------------------------
class GRYCE_API Terrain : public Component {
public:
    // 地形平面尺寸（世界单位）
    float width = 100.0f;
    float depth = 100.0f;

    // 网格分辨率：每边的顶点数 - 1；值越大网格越密
    int resolution = 64;

    // 高度缩放：高度图值 [0,1] 映射到 [0, height_scale]
    float height_scale = 10.0f;

    // 基础纹理路径（空表示使用默认材质）
    std::string base_texture_path;

    // 程序化生成种子；0 表示使用默认起伏
    int seed = 0;

    Terrain();
    ~Terrain() override;

    const char* type() const override { return "Terrain"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 高度图维度（resolution+1）
    int heightmap_size() const;

    // 获取/设置高度图采样（x,z 为 0..resolution 的网格索引）
    float height_at(int x, int z) const;
    void set_height(int x, int z, float h);

    // 规范化高度图到 [0,1]
    void normalize_heights();

    // 使用简单噪声重新生成高度图
    void generate_noise();

    // 将当前高度图构建为 MeshData（含法线/切线/UV）
    assets::MeshData build_mesh_data() const;

    // -----------------------------------------------------------------------
    // GPU 化（渲染管线每帧调用）
    //
    // 参数（width/depth/resolution/height_scale/seed）或高度图变化时才重建
    // MeshData，静止地形每帧只做一次参数比对，无堆分配、无上传。
    // 与粒子/线段/实例化网格相同的约定：顶点数据在主线程准备成 shared_ptr，
    // 上传命令按值捕获后由渲染线程执行（主线程在渲染线程运行期间不碰 GPU）。
    // -----------------------------------------------------------------------
    void prepare_gpu(render::RenderContext* ctx);

    // 丢弃 GPU 网格（资源热重载 / 上下文切换）；主线程调用安全。
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    const render::Material* material() const { return material_.get(); }
    render::Material* ensure_material();

    // 世界空间包围球（视锥剔除）：由 width/depth/height_scale 与世界变换推出，
    // 不需要查询资源 AABB（地形网格是程序化生成的，没有磁盘资源路径）。
    void world_bounds(const math::Matrix4f& world, math::Vector3f& center, float& radius) const;

private:
    std::vector<float> heightmap_;

    void ensure_heightmap();
    math::Vector3f compute_normal(int x, int z) const;
    // 高度图被修改：下次 prepare_gpu 重建网格
    void mark_dirty() { gpu_dirty_ = true; }

    // ---- GPU 状态 ----
    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::unique_ptr<render::Material> material_;
    // 当前待上传/已上传的 CPU 网格；按值捕获进上传命令，保证命令延迟执行时
    // shared_ptr<vector> 仍然有效。
    std::shared_ptr<const assets::MeshData> geometry_;
    // 网格代次：主线程递增。queued_generation_ 记录"已投递上传命令"的代次，
    // 命令在渲染线程执行前不重复投递（否则启动期连续几帧会重复上传同一份网格）。
    // 初值 1 而 queued 为 0，保证首个有效网格一定会被投递上传。
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};
    std::atomic<bool> material_uploaded_{false};
    // 高度图或参数变化标记（主线程）
    bool gpu_dirty_ = true;
    // 参数快照：编辑器直接改公开字段（反射面板）也能触发重建
    float last_width_ = 0.0f;
    float last_depth_ = 0.0f;
    float last_height_scale_ = -1.0f;
    int last_resolution_ = -1;
    int last_seed_ = 0;
    std::string last_texture_path_;
    // 组件析构时置 false，延迟执行的异步上传命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components