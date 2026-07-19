#pragma once

#include <cstdint>

struct ImDrawData;

namespace gryce_engine::render {

class ITexture;

// ---------------------------------------------------------------------------
// IImGuiBackend — Dear ImGui 渲染后端抽象
// 负责 API 相关的设备对象创建、每帧 NewFrame、绘制 DrawData。
// 平台相关部分（GLFW）由 ImGuiRenderer 统一处理。
// ---------------------------------------------------------------------------
class IImGuiBackend {
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
    // 面板 ImGui::Image 采样）。返回 0 表示该后端暂不支持
    // （Vulkan 端的 descriptor 注册留待后续实现）。
    virtual uint64_t imgui_texture_id(ITexture* texture) const {
        (void)texture;
        return 0;
    }
};

} // namespace gryce_engine::render
