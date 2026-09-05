#pragma once

#include <cstdint>
#include <limits>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RHI 资源句柄系统
// ---------------------------------------------------------------------------
// 用轻量句柄替代裸指针引用 GPU 资源，消除跨线程释放与悬空指针风险。
// 句柄只含 index + generation + type，实际对象由后端 ResourcePool 管理。

enum class RHIResourceType : uint8_t {
    None = 0,
    Mesh,
    Texture,
    Shader,
    Framebuffer,
    Buffer,
    Count
};

struct RHIHandle {
    uint32_t index = invalid_index;
    uint32_t generation = 0;
    RHIResourceType type = RHIResourceType::None;

    static constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();

    bool is_valid() const { return index != invalid_index && type != RHIResourceType::None; }
    bool operator==(const RHIHandle& other) const {
        return index == other.index && generation == other.generation && type == other.type;
    }
    bool operator!=(const RHIHandle& other) const { return !(*this == other); }
};

template<RHIResourceType Type>
struct RHITypedHandle {
    uint32_t index = RHIHandle::invalid_index;
    uint32_t generation = 0;

    static constexpr RHIResourceType type = Type;

    bool is_valid() const { return index != RHIHandle::invalid_index; }
    RHIHandle to_generic() const { return {index, generation, Type}; }
    bool operator==(const RHITypedHandle& other) const {
        return index == other.index && generation == other.generation;
    }
    bool operator!=(const RHITypedHandle& other) const { return !(*this == other); }
};

using RHIMeshHandle = RHITypedHandle<RHIResourceType::Mesh>;
using RHITextureHandle = RHITypedHandle<RHIResourceType::Texture>;
using RHIShaderHandle = RHITypedHandle<RHIResourceType::Shader>;
using RHIFramebufferHandle = RHITypedHandle<RHIResourceType::Framebuffer>;
using RHIBufferHandle = RHITypedHandle<RHIResourceType::Buffer>;

} // namespace gryce_engine::render
