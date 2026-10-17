#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "assets/mesh_data.h"
#include "assets/mesh_lod.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/render2d.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// MeshRenderer — 3D 网格渲染组件
// 通过 res:/ 路径引用模型资源，上传 GPU 后渲染。
//
// 自动 LOD（默认开启）：
//   首次绘制时向 AssetManager 申请该模型的 LOD 链（见 assets/mesh_lod.h），
//   之后每帧按"相机距离 → 屏幕空间几何误差"选择级别。级别不是写死的距离档，
//   而是由每一级的实测几何误差反推出来的（见 update_lod）：
//
//     误差在屏幕上占的像素 = error * 0.5*height / (距离 * tan(fov/2))
//     该值 <= lod_pixel_error 时才允许切到这一级
//
//   因此 fov / 分辨率 / 视距自动适配：拉近镜头会自动回到高模，FOV 变大
//   （广角）同样会自动回高模 —— 这正是"按距离算细分范围"的正确形式。
//   lod_bias 是给美术的偏置：>1 更晚切低模（保画质），<1 更早切（保性能）。
//   lod_distances 非空时改用显式世界单位距离阈值（硬性区间，忽略误差推导）。
//
//   近处（level 0）永远是原始网格；低模只在"看不出差别"的距离上生效。
//   阴影 / 深度预通道跟随当前级别，低模同时省掉阴影 pass 的几何开销。
// ---------------------------------------------------------------------------
class GRYCE_API MeshRenderer : public Component {
public:
    std::string mesh_path;
    // 多子网格模型（FBX/GLB）：本实体渲染第几个子网格。
    // -1 = 整体（把全部子网格合并成一个 MeshData，材质取第一个有效材质）。
    // 多材质模型由装配层按子网格拆成多个实体，各自指向自己的子网格，
    // 从而带上各自的材质与贴图（见 scene/multi_material_assembly.h）。
    int submesh_index = -1;
    std::unique_ptr<render::Material> material;
    // 广告牌（Sprite3D）：忽略自身旋转，始终面向相机（局部 +Z 朝向相机）
    bool billboard = false;
    // 关闭深度测试的网格（FPS 武器等 viewmodel）：始终绘制在场景之上。
    bool depth_test = true;

    // ---- 自动 LOD ----
    // 关掉则永远画 level 0（可逐个物体控制；全局可用 GRYCE_NO_LOD=1 关闭）。
    bool lod_enabled = true;
    // 距离偏置：>1 更晚切低模（保画质），<1 更早切低模（保性能）
    float lod_bias = 1.0f;
    // 允许的最大屏幕空间几何误差（像素）。越小越晚切低模。
    float lod_pixel_error = 2.0f;
    // 显式距离阈值（世界单位，按 LOD 级数递增）。非空时优先于误差推导。
    // 例：{40, 120, 320} 表示距离 >= 40 用 L1、>= 120 用 L2、>= 320 用 L3。
    std::vector<float> lod_distances;

    MeshRenderer();
    explicit MeshRenderer(const std::string& path);
    ~MeshRenderer() override;

    const char* type() const override { return "MeshRenderer"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 确保 material 非空
    render::Material* ensure_material();

    // 上传/获取 GPU mesh（缓存，幂等）。上传的是 level 0（原始网格）。
    // allow_while_running=true 允许在渲染线程运行时上传（供渲染系统内部使用）。
    render::IMesh* upload_to_gpu(render::RenderContext* ctx, const assets::MeshData* data,
                                 bool allow_while_running = false);

    // 当前已上传的 GPU mesh 句柄（恒为 level 0 / 原始网格）。
    // 视锥剔除与包围球缓存用它，保证切换 LOD 时剔除判据不抖动。
    render::RHIMeshHandle gpu_mesh_handle() const {
        return gpu_mesh_handle_;
    }

    // 本帧该画的网格句柄：LOD 已选中的低模（若已就绪），否则回退 level 0。
    render::RHIMeshHandle draw_mesh_handle() const;

    // ---- 空间分块 LOD 的绘制接口（见 assets/mesh_lod.h 的 ChunkedLod 注释）----
    // 本帧要提交几次绘制：未分块 / 分块尚未全部就绪 = 1（画整模，行为与今天一致），
    // 分块就绪 = 块数（每块按自己的距离选了级别）。
    // 之所以"全就绪才切"：一部分块 + 整模混合绘制会把同一片几何画两遍。
    int draw_mesh_count() const;
    // 第 i 个待绘制网格的句柄（越界/未就绪时回退整模）
    render::RHIMeshHandle draw_mesh_handle_at(int i) const;
    // 第 i 个待绘制网格的局部包围球（排序与逐块视锥剔除用）
    void draw_mesh_local_bounds(int i, math::Vector3f& local_center, float& local_radius) const;
    // 第 i 个待绘制网格的世界包围球（含世界变换的缩放）
    void chunk_world_bounds(int i, const math::Matrix4f& world, math::Vector3f& center,
                            float& radius) const;
    bool chunked_lod_active() const { return chunked_draw_ && chunks_ready(); }
    bool chunked_lod_available() const { return chunked_ && chunks_ && !chunks_->empty(); }
    int chunk_count() const {
        return (chunked_ && chunks_) ? static_cast<int>(chunks_->size()) : 0;
    }

    // 当前已上传的 GPU mesh（线程安全：仅当上传完全完成后才返回非空）
    render::IMesh* gpu_mesh() const {
        return (ctx_ && uploaded_.load(std::memory_order_acquire)) ? ctx_->mesh(gpu_mesh_handle_) : nullptr;
    }

    // ---- LOD 运行时接口（由 RenderPipeline 每帧调用）----
    // 已生成的 LOD 级数（0 = 尚未生成 / 该模型不做 LOD）
    int lod_level_count() const {
        return lod_chain_ ? lod_chain_->level_count() : 0;
    }
    int active_lod_level() const { return active_lod_; }
    const assets::LodChain* lod_chain() const { return lod_chain_.get(); }

    // 按相机位置选择 LOD 级别，并在需要时投递该级别的 GPU 上传命令。
    // 纯 CPU（只计算与排队），渲染线程运行时从主线程调用安全。
    void update_lod(render::RenderContext* ctx, const math::Matrix4f& world,
                    const math::Vector3f& cam_pos, float tan_half_fov, int viewport_height);

    // 生命周期 token：异步上传命令捕获 MeshRenderer* 后可能延迟 1~3 帧
    // 才在渲染线程执行；token 供回调在执行前校验组件是否仍存活，防止 UAF。
    std::shared_ptr<std::atomic<bool>> alive_token() const { return alive_token_; }

    // 资源热重载：销毁当前 GPU mesh 与材质贴图（push 到渲染线程延迟销毁），
    // 并重置 uploaded_，使下一帧渲染系统重新从 AssetManager 拉取最新数据上传。
    // 主线程调用安全（仅排队销毁命令，无 GPU 调用）。
    void invalidate_gpu_mesh();

private:
    // 为该级别投递上传命令（幂等：已上传/已排队直接返回）
    void request_lod_upload(render::RenderContext* ctx, int level);
    // 丢弃所有 LOD 级别的 GPU 网格（ctx 已失效 / 热重载）
    void release_lod_meshes();
    // 世界包围球：用 LOD 链的局部 AABB + 世界变换，与当前绘制的级别无关
    void world_lod_bounds(const math::Matrix4f& world, math::Vector3f& center, float& radius) const;

    // ---- 空间分块 ----
    struct ChunkState {
        int active = 0;
        std::array<render::RHIMeshHandle, assets::k_max_lod_levels> meshes{};
        std::atomic<uint32_t> uploaded{0};
        std::atomic<uint32_t> pending{0};
    };
    bool chunks_ready() const;
    void release_chunk_meshes();
    void request_chunk_upload(render::RenderContext* ctx, std::size_t chunk, int level);
    // 单个包围球的选级（逐物体与逐块共用）：只算，不改任何状态
    int select_level_for(const assets::LodChain& chain, const math::Vector3f& world_center,
                         float world_radius, float error_scale, const math::Vector3f& cam_pos,
                         float tan_half_fov, int viewport_height, int current_level) const;

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    // 渲染线程完成上传后设置，主线程通过 acquire 语义读取，
    // 保证 gpu_mesh_ 和 material 纹理指针对主线程可见。
    std::atomic<bool> uploaded_{false};

    // ---- LOD 状态 ----
    std::shared_ptr<const assets::LodChain> lod_chain_;
    // 各级 GPU 句柄；位掩码记录"已上传/已排队"，避免主线程依赖非原子句柄判状态。
    std::array<render::RHIMeshHandle, assets::k_max_lod_levels> lod_meshes_{};
    std::atomic<uint32_t> lod_uploaded_mask_{0};
    std::atomic<uint32_t> lod_pending_mask_{0};
    int active_lod_ = 0;
    // 整模（逐物体）路径自己的级别，带滞回；active_lod_ 是"本帧实际画的"级别，
    // 分块接管时会被改成分块里的最高级。
    int whole_lod_ = 0;
    // LOD 链已尝试生成过（失败也不反复申请）
    bool lod_chain_resolved_ = false;

    // ---- 空间分块 LOD 状态 ----
    std::shared_ptr<const assets::ChunkedLod> chunked_;
    // 用 shared_ptr 持有状态：在途上传命令捕获同一份快照，invalidate 重建后
    // 旧命令只会写进已脱管的 vector，不会踩已释放内存（见 request_chunk_upload）。
    std::shared_ptr<std::vector<std::unique_ptr<ChunkState>>> chunks_;
    // 每次重建/释放块状态都自增，在途命令据此判断自己是否已过期
    std::atomic<uint32_t> chunk_generation_{0};
    // 诊断计数：分块上传成功次数（GRYCE_LOD_DEBUG 时打印）
    std::atomic<uint32_t> chunk_upload_ok_{0};
    bool chunked_resolved_ = false;
    // 本帧是否真的用分块绘制。判据见 update_lod：只有"逐块选级后的三角形总数
    // 明显少于整模当前级别的三角形数"时才切分块 —— 否则整模一次画完更省
    // （1 次绘制 vs 24 次，且不用为每块的 L0 再占一份显存）。
    bool chunked_draw_ = false;

    // 组件析构时置 false，延迟执行的异步上传命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components
