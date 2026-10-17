#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "export.h"
#include "assets/asset.h"
#include "assets/asset_handle.h"
#include "assets/async_loader.h"
#include "assets/mesh_data.h"
#include "assets/mesh_lod.h"
#include "assets/skinned_mesh_data.h"
#include "assets/texture_data.h"
#include "resources/pak_bundle.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssetManager — 通用资源管理器（CPU 侧资源缓存）
// 按 res:/ 路径缓存 Asset，避免重复导入。
// ---------------------------------------------------------------------------
class GRYCE_API AssetManager {
public:
    static AssetManager& instance();

    // 通用同步加载接口
    template<typename T>
    AssetHandle<T> load(const std::string& path);

    // 异步加载接口，加载完成后通过回调通知
    template<typename T>
    void load_async(const std::string& path,
                    std::function<void(AssetHandle<T>)> on_complete = nullptr);

    // 兼容旧 API：加载/获取网格资源。
    // 返回共享指针持有资源，避免 LRU 驱逐在调用方仍持有时释放内存。
    //
    // submesh_index < 0：整体网格 —— 多子网格模型按"顶点重排 + 索引偏移"合并成
    //   单一 MeshData，材质只取第一个有效材质（引擎 MeshRenderer 单网格单材质）。
    // submesh_index >= 0：取模型文件里的第 N 个子网格，各自保留自己的材质与贴图
    //   （多材质模型由装配层按子网格拆成多个实体，见 scene/multi_material_assembly.h）。
    //   越界返回 nullptr。
    std::shared_ptr<const MeshData> load_mesh(const std::string& path, int submesh_index = -1);
    std::shared_ptr<const MeshData> load_mesh_shared(const std::string& path, int submesh_index = -1);

    // 自动 LOD 链（见 assets/mesh_lod.h）。
    // 首次调用时按默认设置生成并缓存（纯 CPU，1.4M 三角形 ~0.2s，只在首次命中时付一次），
    // 之后按 (path, submesh) 直接返回缓存；levels[0] 就是 load_mesh 返回的同一份网格。
    // 返回 nullptr 表示该网格不适合做 LOD（太小 / 生成失败 / GRYCE_NO_LOD=1）。
    std::shared_ptr<const LodChain> load_mesh_lods(const std::string& path, int submesh_index = -1);
    // 空间分块 LOD（见 ChunkedLod 注释）：把大模型切成若干块，每块各自一条链。
    // 只有"大到值得分块"的网格才生成（默认 >= 120k 三角形，可用
    // GRYCE_CHUNK_LOD_MIN_TRIS 覆盖）；返回 nullptr 表示该网格走逐物体 LOD。
    // cells_per_axis <= 0 表示按目标块大小自动决定（choose_chunk_cells）。
    std::shared_ptr<const ChunkedLod> load_mesh_chunk_lods(const std::string& path,
                                                           int submesh_index = -1,
                                                           int cells_per_axis = 0);
    // 丢弃 LOD 链缓存（资源热重载 / unload 时自动调用）。
    void clear_lod_cache();

    // 逐子网格加载：返回模型文件里的每个子网格（各自带自己的材质），不做合并。
    // 返回的元素与 load_mesh(path, i) 返回的是同一批对象（同一份缓存，不额外驻留）。
    // 加载失败返回空 vector。
    std::vector<std::shared_ptr<const MeshData>> load_meshes(const std::string& path);

    // 带骨骼/动画的模型（.gltf/.fbx，走 Assimp import_skinned）。
    // 返回缓存的共享指针；失败返回 nullptr。需要 GRYCE_HAS_ASSIMP。
    std::shared_ptr<SkinnedModelData> load_skinned_model(const std::string& path);
    // 异步版本：import_skinned（纯 CPU）在 AsyncLoader 工作线程执行，
    // 完成后回调（经 AsyncLoader::poll 在主线程触发）拿到缓存指针。
    void load_skinned_model_async(const std::string& path,
                                  std::function<void(std::shared_ptr<SkinnedModelData>)> on_complete = nullptr);

    // 手动释放资源
    void unload(const std::string& path);
    void unload_mesh(const std::string& path);
    void clear();

    bool has(const std::string& path) const;
    bool has_mesh(const std::string& path) const;

    // 资源热重载：检查缓存资源的源文件（res:/ 路径解析后的实际文件）是否变化，
    // 有变化则重新导入并替换缓存。返回本次发生变化的 res:/ 路径列表，
    // 调用方据此失效 GPU 副本（MeshRenderer::invalidate_gpu_mesh 等）。
    // 主线程调用安全（纯 CPU 导入）。
    std::vector<std::string> poll_hot_reload();

    // 异步加载状态查询
    LoadingState get_async_state(const std::string& path) const;
    bool is_async_loading(const std::string& path) const;

    // 缓存限制（0 表示不限）
    void set_max_cache_count(size_t count);
    void set_max_cache_memory_mb(float mb);
    size_t max_cache_count() const;
    float max_cache_memory_mb() const;

    // 当前缓存统计
    size_t resident_count() const;
    size_t resident_memory_bytes() const;

    // 挂载 .gpack 资源包；返回 bundle id（失败返回 -1）
    int mount_bundle(const std::string& pack_path);
    void unmount_bundle(int id);

    /// Returns a path that can be opened with std::ifstream: the real file
    /// when it exists, otherwise a temp extraction from a mounted bundle.
    /// Returns an empty string when the resource is not found.
    // 解析为可读路径：磁盘文件优先，否则从已挂载 bundle 提取到临时目录。
    // 调用方必须持有 mutex_（内部会访问 bundles_）。
    std::string resolve_for_reading(const std::string& path);

    // 线程安全的封装：内部加锁后调用 resolve_for_reading。渲染线程等在未持有
    // mutex_ 的场景解析资源（如 shader 源码两级解析）使用本接口。
    std::string resolve_any(const std::string& path);

private:
    AssetManager() = default;

    struct MountedBundle {
        int id = 0;
        std::unique_ptr<resources::PakReader> pak_reader;
        std::unordered_map<std::string, std::string> extracted_temp_paths;
    };

    struct CacheEntry {
        std::shared_ptr<Asset> asset;
        size_t memory_size = 0;
        std::chrono::steady_clock::time_point last_access;
        // 热重载：源文件（resolved 实际路径）与其最后修改时间
        std::string source_path;
        std::filesystem::file_time_type source_mtime;
    };

    // 网格逐子网格缓存：一个模型文件（FBX/GLB）里的每个子网格独立持有，
    // 各自带自己的材质（合并视图见 assets_，按需构建）。
    struct MeshGroupEntry {
        std::vector<std::shared_ptr<MeshData>> meshes;
        std::string source_path;
        std::filesystem::file_time_type source_mtime;
    };

    // 加载具体资源类型（特化实现）
    std::shared_ptr<MeshData> load_mesh_internal(const std::string& path);
    // 逐子网格导入（未命中缓存则导入并写入 mesh_groups_）。调用方必须持有 mutex_。
    std::vector<std::shared_ptr<MeshData>> load_mesh_group_unlocked(const std::string& path);
    // 按扩展名选择导入器（OBJ 内置 / 其他 Assimp）。调用方必须持有 mutex_。
    std::vector<MeshData> import_mesh_file(const std::string& path, const std::string& resolved);
    // 解析网格源文件：磁盘优先，已挂载 bundle 内提取兜底。调用方必须持有 mutex_。
    std::string resolve_mesh_source_unlocked(const std::string& path);
    std::shared_ptr<TextureData> load_texture_internal(const std::string& path);
    std::shared_ptr<SkinnedModelData> load_skinned_model_internal(const std::string& path);

    void touch_unlocked(const std::string& path);
    void maybe_evict_unlocked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> assets_;
    // 逐子网格视图（多材质模型按子网格拆分渲染时使用）
    std::unordered_map<std::string, MeshGroupEntry> mesh_groups_;
    // 自动 LOD 链缓存，键为 "path#submesh"（与 mesh_groups_ 分开存，
    // 避免 LOD 生成逻辑侵入导入路径本身）
    std::unordered_map<std::string, std::shared_ptr<const LodChain>> lod_chains_;
    std::unordered_map<std::string, std::shared_ptr<const ChunkedLod>> chunk_lods_;
    // SkinnedModelData 不是 Asset 子类（值语义聚合），独立缓存
    std::unordered_map<std::string, std::shared_ptr<SkinnedModelData>> skinned_models_;

    // 已挂载的 .gpack 资源包
    std::unordered_map<int, MountedBundle> bundles_;
    int next_bundle_id_ = 1;
    std::string extract_from_bundle_unlocked(const std::string& path);

    size_t max_count_ = 0;
    size_t max_memory_bytes_ = 0;
};

// ---------------------------------------------------------------------------
// 显式特化声明（必须标记 GRYCE_API 以便 DLL 导出）
template<>
GRYCE_API AssetHandle<MeshData> AssetManager::load<MeshData>(const std::string& path);

template<>
GRYCE_API AssetHandle<TextureData> AssetManager::load<TextureData>(const std::string& path);

// 异步加载特化声明
template<>
GRYCE_API void AssetManager::load_async<MeshData>(const std::string& path,
                                                   std::function<void(AssetHandle<MeshData>)> on_complete);

template<>
GRYCE_API void AssetManager::load_async<TextureData>(const std::string& path,
                                                      std::function<void(AssetHandle<TextureData>)> on_complete);

} // namespace gryce_engine::assets
