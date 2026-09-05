#pragma once

#include <memory>
#include <type_traits>

#include "assets/asset.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssetHandle<T> — 类型安全的资源句柄
// 内部持有 std::shared_ptr<T>，T 必须继承 Asset。
// ---------------------------------------------------------------------------
template<typename T>
class AssetHandle {
public:
    static_assert(std::is_base_of_v<Asset, T>, "T must derive from Asset");

    AssetHandle() = default;
    explicit AssetHandle(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

    T* get() const { return ptr_.get(); }
    T* operator->() const { return ptr_.get(); }
    T& operator*() const { return *ptr_; }

    explicit operator bool() const { return ptr_ != nullptr; }
    bool valid() const { return ptr_ != nullptr; }

    void reset() { ptr_.reset(); }

    std::shared_ptr<T> shared() const { return ptr_; }

    bool operator==(const AssetHandle<T>& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const AssetHandle<T>& other) const { return ptr_ != other.ptr_; }

private:
    std::shared_ptr<T> ptr_;
};

} // namespace gryce_engine::assets
