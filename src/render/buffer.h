#pragma once

#include <cstddef>
#include <cstdint>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 缓冲用途标记
// ---------------------------------------------------------------------------
enum class BufferUsage {
    Static,     // 一次写入，多次读取
    Dynamic,    // 频繁更新
    Stream      // 每帧更新
};

// ---------------------------------------------------------------------------
// IBuffer — GPU 缓冲（SSBO / UBO）接口
// 用于存储光照数据、骨骼矩阵等需要 shader 随机访问的数据。
// ---------------------------------------------------------------------------
class IBuffer {
public:
    virtual ~IBuffer() = default;

    // 创建/重新创建缓冲
    virtual bool create(size_t size, const void* data, BufferUsage usage) = 0;

    // 更新缓冲数据（子区域）
    virtual void update(const void* data, size_t size, size_t offset = 0) = 0;

    // 绑定到指定的 binding point（适用于 SSBO）
    virtual void bind(uint32_t binding_point) const = 0;

    // 销毁
    virtual void destroy() = 0;

    // 查询
    virtual size_t size() const = 0;
    virtual bool is_valid() const = 0;
};

} // namespace gryce_engine::render