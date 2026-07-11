#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "assets/asset.h"
#include "assets/asset_handle.h"
#include "assets/async_loader.h"
#include "assets/mesh_data.h"
#include "assets/texture_data.h"

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

    // 手动释放资源
    void unload(const std::string& path);
    void unload_mesh(const std::string& path);
    void clear();

    bool has(const std::string& path) const;
    bool has_mesh(const std::string& path) const;

    // 异步加载状态查询
    LoadingState get_async_state(const std::string& path) const;
    bool is_async_loading(const std::string& path) const;

private:
    AssetManager() = default;

    // 加载具体资源类型（特化实现）
    std::shared_ptr<MeshData> load_mesh_internal(const std::string& path);
    std::shared_ptr<TextureData> load_texture_internal(const std::string& path);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Asset>> assets_;
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
