#pragma once

#include <cstddef>
#include <imgui.h>
#include <string>

// ---------------------------------------------------------------------------
// EngineTheme::ModernDark — 暗色主题（受现代桌面系统设计语言启发）
//
// 直接复制到项目可用：包含 Apply() + 控件辅助函数 + 字体加载。
// ---------------------------------------------------------------------------

namespace EngineTheme {

namespace ModernDark {

// 精确色值（受现代桌面系统启发）
constexpr ImU32 WindowBackground   = IM_COL32(0x1E, 0x1E, 0x1E, 0xFF); // #1E1E1E
constexpr ImU32 ContentBackground  = IM_COL32(0x2D, 0x2D, 0x2D, 0xFF); // #2D2D2D
constexpr ImU32 ContentBackground2 = IM_COL32(0x32, 0x32, 0x32, 0xFF); // #323232
constexpr ImU32 Accent             = IM_COL32(0x0A, 0x84, 0xFF, 0xFF); // #0A84FF
constexpr ImU32 Separator          = IM_COL32(0x3A, 0x3A, 0x3C, 0xFF); // #3A3A3C
constexpr ImU32 TextPrimary        = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF); // #FFFFFF
constexpr ImU32 TextSecondary      = IM_COL32(0x98, 0x98, 0x9D, 0xFF); // #98989D
constexpr ImU32 TextDisabled       = IM_COL32(0x5C, 0x5C, 0x5F, 0xFF); // #5C5C5F
constexpr ImU32 Green              = IM_COL32(0x30, 0xD1, 0x58, 0xFF); // #30D158
constexpr ImU32 Yellow             = IM_COL32(0xFF, 0xD6, 0x0A, 0xFF); // #FFD60A
constexpr ImU32 Red                = IM_COL32(0xFF, 0x45, 0x3A, 0xFF); // #FF453A

// 半透明 / vibrancy 层
constexpr ImU32 PanelTranslucent   = IM_COL32(0x2D, 0x2D, 0x2D, 0xD0); // ~82% alpha
constexpr ImU32 ModalDim           = IM_COL32(0x00, 0x00, 0x00, 0x78); // ~47% alpha

// 应用主题到当前 ImGui 上下文
void Apply();

} // namespace ModernDark

// ---------------------------------------------------------------------------
// EngineTheme::ModernLight — 亮色主题（受现代移动/桌面系统设计语言启发）
//
// 直接复制到项目可用：包含 Apply() + 控件辅助函数 + 字体加载。
// ---------------------------------------------------------------------------

namespace ModernLight {

// 精确色值（从现代移动/桌面系统提取）
constexpr ImU32 WindowBackground   = IM_COL32(0xF5, 0xF5, 0xF7, 0xFF); // #F5F5F7
constexpr ImU32 ContentBackground  = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF); // #FFFFFF
constexpr ImU32 SecondaryPanel     = IM_COL32(0xF2, 0xF2, 0xF7, 0xFF); // #F2F2F7
constexpr ImU32 Accent             = IM_COL32(0x00, 0x7A, 0xFF, 0xFF); // #007AFF
constexpr ImU32 Separator          = IM_COL32(0xE5, 0xE5, 0xEA, 0xFF); // #E5E5EA
constexpr ImU32 TextPrimary        = IM_COL32(0x00, 0x00, 0x00, 0xFF); // #000000
constexpr ImU32 TextSecondary      = IM_COL32(0x8E, 0x8E, 0x93, 0xFF); // #8E8E93
constexpr ImU32 TextDisabled       = IM_COL32(0xC7, 0xC7, 0xCC, 0xFF); // #C7C7CC
constexpr ImU32 Green              = IM_COL32(0x34, 0xC7, 0x59, 0xFF); // #34C759
constexpr ImU32 Yellow             = IM_COL32(0xFF, 0xCC, 0x00, 0xFF); // #FFCC00
constexpr ImU32 Red                = IM_COL32(0xFF, 0x3B, 0x30, 0xFF); // #FF3B30

// 半透明 / vibrancy 层
constexpr ImU32 PanelTranslucent   = IM_COL32(0xFF, 0xFF, 0xFF, 0xD9); // ~85% alpha
constexpr ImU32 SelectedFill       = IM_COL32(0xE3, 0xF2, 0xFF, 0xFF); // #E3F2FF
constexpr ImU32 ModalDim           = IM_COL32(0x00, 0x00, 0x00, 0x4D); // ~30% alpha

// 交互状态色
constexpr ImU32 AccentHover        = IM_COL32(0x00, 0x66, 0xD4, 0xFF); // 强调色悬停变暗
constexpr ImU32 AccentActive       = IM_COL32(0x00, 0x5B, 0xBF, 0xFF); // 强调色按下变暗

// 应用主题到当前 ImGui 上下文
void Apply();

} // namespace ModernLight

// ---------------------------------------------------------------------------
// 动态字号档位
// ---------------------------------------------------------------------------
enum class FontSizeClass { Small, Medium, Large };

// ---------------------------------------------------------------------------
// 现代系统风格控件辅助函数（在 EngineTheme 命名空间下）
// ---------------------------------------------------------------------------

// 侧边栏条目：38px 高，左侧 4px 圆角指示条 + 图标 + 文字（暗色主题风格）
// icon_text 为字体图标字符（受现代系统图标风格启发）
bool SidebarItem(const char* label,
                 const char* icon_text,
                 bool selected,
                 ImU32 indicator_color = ModernDark::Accent,
                 const ImVec2& size_arg = ImVec2(0, 38.0f));

// 亮色主题侧边栏条目：28px 大图标 + 文字，选中项整行浅蓝背景
bool SidebarItemLight(const char* label,
                      const char* icon_text,
                      bool selected,
                      const ImVec2& size_arg = ImVec2(0, 44.0f));

// 工具栏按钮：40px 区域，22px 图标，透明背景
bool ToolbarButton(const char* id,
                   const char* icon_text,
                   const ImVec2& size = ImVec2(36.0f, 36.0f));

// 导航栏：顶部 44px，左侧返回箭头 + 标题
// back_clicked 输出是否点击返回按钮；返回 true 表示标题区域被点击（通常不使用）
bool NavBar(const char* title, bool* back_clicked = nullptr, float height = 44.0f);

// 列表行：48px 高度，选中项整行浅蓝背景 + 圆角 10px（受现代系统启发）
bool ListRow(const char* label, bool selected, float height = 48.0f);

// 搜索框：圆角 10px，左侧放大镜图标，placeholder 提示文字
bool SearchField(const char* label,
                 char* buf,
                 size_t buf_size,
                 const char* hint = "Search...",
                 float width = -1.0f);

// 分段控件：胶囊形状，选中项填充强调色
// items 为字符串数组，items_count 为数量
bool SegmentedControl(const char* label,
                      int* current_item,
                      const char* const items[],
                      int items_count,
                      float width = -1.0f);

// 开关：胶囊滑块，绿色开启状态
bool ToggleSwitch(const char* label, bool* v);

// 填充按钮：强调色背景 + 白色文字，圆角 7px
bool FilledButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// 描边按钮：白底 + 强调色边框，圆角 7px
bool OutlineButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// 文字按钮：纯文字 + 强调色
bool TextButton(const char* label);

// 卡片：白色背景 + 轻微阴影，圆角 12px
// 调用前需用 ImGui::BeginGroup()/EndGroup() 包裹内容，或传入自定义绘制回调
void BeginCard(const char* id, const ImVec2& size = ImVec2(0, 0));
void EndCard();

// 加载系统风格字体：SF Pro Text / SF Mono（不存在时回退到 Inter / JetBrains Mono）
// size_class 控制 Small / Medium / Large 三档字号
bool LoadModernFonts(FontSizeClass size_class = FontSizeClass::Medium);

// 显式指定字号加载（保留兼容性）
bool LoadModernFonts(float ui_size, float mono_size);

// 获取加载后的 UI / 代码字体（失败返回 nullptr）
ImFont* ModernUIFont();
ImFont* ModernMonoFont();

} // namespace EngineTheme
