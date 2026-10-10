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

    // 标记该缓冲会被着色器以 SSBO 方式访问（可选能力，默认忽略）。
    // D3D12 的硬性约束：**只有 DEFAULT 堆资源才能当 UAV（读写 SSBO）**，UPLOAD 堆
    // 资源加 ALLOW_UNORDERED_ACCESS 会被 CreateCommittedResource 直接拒绝。所以
    // 后端必须在这一步之前知道"这是一块 SSBO"，才能选对堆类型与资源标志。
    // 必须在 create() 之前调用。GL/Vulkan 不需要（它们允许 UPLOAD/持久映射当 SSBO）。
    virtual void set_storage(bool /*read_write*/) {}

    // 更新缓冲数据（子区域）
    virtual void update(const void* data, size_t size, size_t offset = 0) = 0;

    // 同步回读缓冲内容（诊断 / 截图用）。注意会造成 GPU→CPU 停顿，不要放在
    // 每帧热路径上。默认不支持，需要的后端自行实现。
    virtual bool readback(void* dst, size_t size, size_t offset = 0) {
        (void)dst; (void)size; (void)offset;
        return false;
    }

    // 绑定到指定的 binding point（适用于 SSBO）
    virtual void bind(uint32_t binding_point) const = 0;

    // 销毁
    virtual void destroy() = 0;

    // 查询
    virtual size_t size() const = 0;
    virtual bool is_valid() const = 0;
};

} // namespace gryce_engine::render