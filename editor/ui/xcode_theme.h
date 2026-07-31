#pragma once

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// Xcode / Apple HIG 风格主题（替换编辑器默认的 暗色 / 亮色 预设）
//
//   apply_xcode_dark()  — Xcode 暗色：#25262B 窗口 / #1E1F24 内容 / #16171B 内嵌，
//                         强调色 #0A84FF，高亮 #409CFF
//   apply_xcode_light() — Xcode 亮色：#F5F5F7 窗口 / #FFFFFF 内容 / #FAFAFA 内嵌
//                         + 细描边，文本 #1D1D1F，次要 #6E6E73，强调色 #0A84FF
//
// 两者共用同一套尺寸（扁平、细描边、~5-6px 圆角、宽松留白）。
// 仅设置 ImGuiStyle，不触碰字体（沿用编辑器现有 CJK 字体加载）。
// ---------------------------------------------------------------------------
void apply_xcode_dark();
void apply_xcode_light();

} // namespace gryce_engine::editor
