#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// Asset — 所有 CPU 侧资源的基类
// 采用 std::shared_ptr 引用计数管理生命周期。
// ---------------------------------------------------------------------------
class Asset : public std::enable_shared_from_this<Asset> {
public:
    virtual ~Asset() = default;

    // 资源类型标识
    virtual const char* type() const = 0;

    // 资源路径（res:/path 或原始路径）
    const std::string& path() const { return path_; }
    void set_path(const std::string& path) { path_ = path; }

    // 当前引用计数（调试用）
    uint64_t ref_count() const {
        return static_cast<uint64_t>(shared_from_this().use_count());
    }

protected:
    Asset() = default;

private:
    std::string path_;
};

} // namespace gryce_engine::assets
