// ===========================================================================
// engine/ui/ui_texture_v_flip.h — 后端纹理采样约定（两条 UI 轨共用）
//
// 标志本后端的纹理第 0 行是图像的**下**边（true，OpenGL 的 framebuffer 原点
// 在左下）还是**上**边（false，Vulkan / D3D12）。GUi_Image 与内置轨的
// BuiltinUi::image 都据此决定 sprite 的 v 方向。
//
// 由 render_api.cpp 在 GRender_Init 里写一次，此后只读：
//   * ImGui 轨问 IImGuiBackend::texture_needs_v_flip；
//   * 内置轨问 IRenderBackend::texture_needs_v_flip（内置轨不建 ImGui 后端，
//     拿不到前者）。
// 为什么必须缓存而不能现场问渲染器：UI 回调运行在 g_renderer.mutex 已持有的
// 路径上，回调内调任何 GRender_* 都会二次加锁而死锁。建后端时写一次、绘制时
// 只读，是唯一安全的做法。
// ===========================================================================

#ifndef GRYCE_UI_TEXTURE_V_FLIP_H
#define GRYCE_UI_TEXTURE_V_FLIP_H

namespace gryce_ui {

void set_texture_v_flip(bool flip);
bool texture_v_flip();

} // namespace gryce_ui

#endif // GRYCE_UI_TEXTURE_V_FLIP_H
