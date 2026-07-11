#pragma once

#include <vulkan/vulkan.h>

#include "render/imgui_backend.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// VulkanImGuiBackend — 使用 imgui_impl_vulkan 绘制 ImGui
// ---------------------------------------------------------------------------
class VulkanBackend;

class VulkanImGuiBackend : public IImGuiBackend {
public:
    explicit VulkanImGuiBackend(class IRenderBackend* backend);
    ~VulkanImGuiBackend() override;

    bool init() override;
    void shutdown() override;
    void new_frame() override;
    void render_draw_data(ImDrawData* draw_data) override;
    bool is_vulkan() const override { return true; }

private:
    VulkanBackend* backend_ = nullptr;
    bool initialized_ = false;
};

} // namespace gryce_engine::render
