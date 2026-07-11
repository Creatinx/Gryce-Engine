#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "assets/asset.h"
#include "assets/asset_handle.h"
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

    // 通用加载接口，返回类型安全的 AssetHandle
    template<typename T>
    AssetHandle<T> load(const std::string& path);

    // 兼容旧 API：加载/获取网格资源
    const MeshData* load_mesh(const std::string& path);
    std::shared_ptr<const MeshData> load_mesh_shared(const std::string& path);

    // 手动释放资源
    void unload(const std::string& path);
    void unload_mesh(const std::string& path);
    void clear();

    bool has(const std::string& path) const;
    bool has_mesh(const std::string& path) const;

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
// ---------------------------------------------------------------------------
template<>
AssetHandle<MeshData> AssetManager::load<MeshData>(const std::string& path);

template<>
AssetHandle<TextureData> AssetManager::load<TextureData>(const std::string& path);

} // namespace gryce_engine::assets
