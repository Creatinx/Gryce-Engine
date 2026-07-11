#pragma once

struct ImDrawData;

namespace gryce_engine::render {

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
};

} // namespace gryce_engine::render
