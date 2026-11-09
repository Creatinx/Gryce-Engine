#pragma once

namespace gryce_engine::ui {
class UIDrawList;
}

namespace gryce_engine::render {

class IRenderer2D;

// 单一 UI 提交路径：UIDrawList -> IRenderer2D overlay。所有图形后端继续复用
// Renderer2D 已有实现，不再为 UI 复制 OpenGL / Vulkan / D3D12 后端。
void render_ui_draw_list(IRenderer2D& renderer, const ui::UIDrawList& draw_list,
                         int screen_width, int screen_height);

} // namespace gryce_engine::render
