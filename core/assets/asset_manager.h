#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "assets/asset.h"
#include "assets/asset_handle.h"
#include "assets/async_loader.h"
#include "assets/mesh_data.h"
#include "assets/skinned_mesh_data.h"
#include "assets/texture_data.h"
#include "resources/gpack_bundle.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssetManager — 通用资源管理器（CPU 侧资源缓存）
// 按 res:/ 路径缓存 Asset，避免重复导入。
// ---------------------------------------------------------------------------
class AssetManager {
public:
    static AssetManager& instance();

    // 通用同步加载接口
    template<typename T>
    AssetHandle<T> load(const std::string& path);

    // 异步加载接口，加载完成后通过回调通知
    template<typename T>
    void load_async(const std::string& path,
                    std::function<void(AssetHandle<T>)> on_complete = nullptr);

    // 兼容旧 API：加载/获取网格资源
    const MeshData* load_mesh(const std::string& path);
    std::shared_ptr<const MeshData> load_mesh_shared(const std::string& path);

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

private:
    AssetManager() = default;

    struct MountedBundle {
        int id = 0;
        std::unique_ptr<resources::GPackReader> reader;
        std::unordered_map<std::string, std::string> extracted_temp_paths;
    };

    struct CacheEntry {
        std::shared_ptr<Asset> asset;
        size_t memory_size = 0;
        std::chrono::steady_clock::time_point last_access;
    };

    // 加载具体资源类型（特化实现）
    std::shared_ptr<MeshData> load_mesh_internal(const std::string& path);
    std::shared_ptr<TextureData> load_texture_internal(const std::string& path);
    std::shared_ptr<SkinnedModelData> load_skinned_model_internal(const std::string& path);

    void touch_unlocked(const std::string& path);
    void maybe_evict_unlocked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> assets_;
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
// 显式特化声明
template<>
AssetHandle<MeshData> AssetManager::load<MeshData>(const std::string& path);

template<>
AssetHandle<TextureData> AssetManager::load<TextureData>(const std::string& path);

// 异步加载特化声明
template<>
void AssetManager::load_async<MeshData>(const std::string& path,
                                         std::function<void(AssetHandle<MeshData>)> on_complete);

template<>
void AssetManager::load_async<TextureData>(const std::string& path,
                                            std::function<void(AssetHandle<TextureData>)> on_complete);
// ---------------------------------------------------------------------------
template<>
AssetHandle<MeshData> AssetManager::load<MeshData>(const std::string& path);

template<>
AssetHandle<TextureData> AssetManager::load<TextureData>(const std::string& path);

} // namespace gryce_engine::assets
