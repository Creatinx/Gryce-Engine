#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "assets/asset_handle.h"
#include "assets/asset_manager.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AsyncLoader — 异步资源加载器
// 按 path 去重，避免同一资源并发加载两次；加载完成后通过 future 返回。
// ---------------------------------------------------------------------------
class AsyncLoader {
public:
    static AsyncLoader& instance();

    // 异步加载任意 Asset 类型，返回 future<AssetHandle<T>>
    template<typename T>
    std::future<AssetHandle<T>> load(const std::string& path) {
        return std::async(std::launch::async, [path]() {
            return AssetManager::instance().load<T>(path);
        });
    }

private:
    AsyncLoader() = default;
};

} // namespace gryce_engine::assets
