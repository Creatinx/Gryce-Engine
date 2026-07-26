#pragma once

#include <cstdint>

#include "export.h"

struct ImDrawData;

namespace gryce_engine::render {

class ITexture;

// ---------------------------------------------------------------------------
// IImGuiBackend — Dear ImGui 渲染后端抽象
// 负责 API 相关的设备对象创建、每帧 NewFrame、绘制 DrawData。
// 平台相关部分（GLFW）由 ImGuiRenderer 统一处理。
//
// 注意：该类会被 std::unique_ptr<IImGuiBackend> 以值形式跨 DLL 边界返回，
// 因此必须导出，否则 MSVC/Windows 下会出现 ABI/vtable 不匹配，导致返回槽
// 地址为 null，进而在 unique_ptr 内部构造 _Compressed_pair 时崩溃。
// ---------------------------------------------------------------------------
class GRYCE_API IImGuiBackend {
public:
    virtual ~IImGuiBackend() = default;

    // 初始化/销毁 API 设备对象
    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // 每帧调用（在 ImGui::NewFrame 之前）
    virtual void new_frame() = 0;

    // 绘制 ImGui 数据
    virtual void render_draw_data(ImDrawData* draw_data) = 0;

    // 是否使用 Vulkan（影响 GLFW 初始化方式）
    virtual bool is_vulkan() const { return false; }

    // 将引擎纹理转换为 ImGui 用户纹理 ID（ImTextureID，供编辑器 Viewport
    // 面板 ImGui::Image 采样）。OpenGL 返回 GLuint 纹理对象 id，Vulkan 返回
    // 缓存的 VkDescriptorSet；返回 0 表示转换失败。
    virtual uint64_t imgui_texture_id(ITexture* texture) const {
        (void)texture;
        return 0;
    }

    // 使指定纹理的 ImGui 缓存失效（如纹理被销毁/重建后）。
    virtual void invalidate_texture(ITexture* texture) { (void)texture; }

    // 运行时重建 ImGui 字体 GPU 纹理（如字体大小变化后）。
    // 调用方需保证当前线程持有正确的 GPU context / device。
    virtual void rebuild_fonts() {}
};

} // namespace gryce_engine::render
