// ===========================================================================
// engine/ui/ui_render_bridge.h — drawlist → 渲染器的桥（内置 UI 渲染路径第 2 阶段）
//
// 解决的问题：ui_drawlist 只记录"画什么"，光栅化还得有人做。引擎已经有了
// 三后端（D3D12 / Vulkan / GL）都实现的 IRenderer2D（draw_rect / draw_text /
// draw_sprite_region / set_scissor，左上原点），内置 UI 不需要新 shader——
// 缺的只是"把命令翻成调用"的这一层，就是本文件。
//
// 为什么中间要插一个 UiRenderer 抽象接口而不是让桥直接调 IRenderer2D：
//   1) 单测可以用 mock 记录调用序列（顺序、裁剪切换、颜色、文本），不碰
//      GPU、不建窗口——CI 里毫秒级验证"桥的行为正确"；
//   2) 桥不认识任何后端类型，后端差异（颜色格式、scissor 是 int 还是 float、
//      纹理句柄怎么打包）全部收在适配器里，加第四个后端只加一个适配器；
//   3) 无窗口宿主（脚本/服务器）可以配一个"什么都不画"或"画到离屏"的
//      UiRenderer，同一份界面逻辑照跑。
//
// 坐标系：与 UiDrawList / IRenderer2D 的 top_left_origin 一致——左上原点、
// +y 向下、单位逻辑像素。
//
// 线程模型：render() 在渲染线程执行（宿主把它包进渲染命令）；UiDrawList
// 由 UI 线程构建、渲染线程只读，两边靠命令缓冲的所有权转移交接（与
// render_api.cpp 推送 ImGui draw data 的同一条规矩）。
// ===========================================================================

#ifndef GRYCE_UI_RENDER_BRIDGE_H
#define GRYCE_UI_RENDER_BRIDGE_H

#include <cstdint>
#include <memory>

#include "ui/ui_drawlist.h"

namespace gryce_engine::render {
class IRenderer2D;
} // namespace gryce_engine::render

namespace gryce_ui {

// ---------------------------------------------------------------------------
// UiRenderer — 渲染桥对渲染器的最低要求
//
// 与 IRenderer2D 的对应关系（适配器负责转换）：
//   ui_draw_rect      → draw_rect
//   ui_draw_text      → draw_text（(x,y) = 行盒左上角；适配器按字体升部比
//                       换算到渲染器的基线约定，见 ui_render_bridge_rd.cpp）
//   ui_draw_sprite    → draw_sprite_region
//   ui_set_scissor    → set_scissor（w<=0 或 h<=0 = 该裁剪域内无可见内容）
//   ui_reset_scissor  → reset_scissor
// ---------------------------------------------------------------------------
class UiRenderer {
public:
    virtual ~UiRenderer() = default;

    virtual void ui_draw_rect(float x, float y, float w, float h, uint32_t rgba) = 0;
    virtual void ui_draw_text(float x, float y, const char* text, float font_size,
                              uint32_t rgba) = 0;
    virtual void ui_draw_sprite(float x, float y, float w, float h, uint64_t texture,
                                float u0, float v0, float u1, float v1,
                                uint32_t rgba) = 0;
    virtual void ui_set_scissor(float x, float y, float w, float h) = 0;
    virtual void ui_reset_scissor() = 0;
};

// ---------------------------------------------------------------------------
// UiRenderBridge — 把命令缓冲翻成 UiRenderer 调用
//
// 语义（均有单测覆盖，见 test/ui_tests.cpp 的 bridge 套件）：
//   * **裁剪状态按命令流的深度字段同步**：每条命令带 clip = 执行后生效的
//     裁剪栈深度；只有深度变化时才切 scissor（深度不变 = 裁剪矩形没变，
//     push/pop 必然改深度）。深度 0 = 无裁剪 → reset_scissor。
//   * **桥自己维护裁剪栈，不回查 drawlist 的 clips_**：clips_ 是构建期的活
//     状态（双缓冲下会被下一帧的构建覆盖），而 PushClip 命令自带相交后的
//     生效矩形，命令流是自描述的。深度增加 → 取命令上的矩形；深度减少 →
//     取桥栈里的父矩形。
//   * **剔除态不传下来**：被裁掉的绘制命令根本不在命令表里（drawlist 侧已
//     丢弃并计 culled），桥看到的每条绘制命令都该被画。
//   * 命令表满被丢弃的命令同样不会到这里；桥只对"表里有的"负责。
//   * 字符串池按偏移取串，桥不拷贝、不修改 drawlist。
// ---------------------------------------------------------------------------
class UiRenderBridge {
public:
    // 渲染整个命令缓冲。renderer 的帧状态（begin_frame/end_frame、混合、
    // 相机）由调用方负责——桥只发绘制调用，不假设帧边界。
    void render(const UiDrawList& list, UiRenderer& renderer);

    // 上一次 render 的统计（测试与调试用）。
    int draw_commands() const { return draws_; }       // 实际下发的绘制命令数
    int scissor_switches() const { return switches_; } // set/reset scissor 次数

private:
    void apply_clip(const UiDrawList& list, uint32_t depth, UiRenderer& renderer);

    // "尚未应用任何裁剪"的哨兵：每轮 render 开头置它，强制第一条命令同步一次
    // 裁剪状态——调用方可能在两次 render 之间动过渲染器的 scissor（比如
    // begin_frame 复位），桥不能假设自己上次的结论还有效。
    static constexpr uint32_t kNoClipApplied = 0xFFFFFFFFu;
    // 桥自己的裁剪栈（与命令流同步重建，不读 drawlist 的 clips_）
    static constexpr int kMaxClips = UiDrawList::kMaxClips;

    UiClipRect clip_stack_[kMaxClips];
    int        stack_depth_   = 0;
    int        draws_         = 0;
    int        switches_      = 0;
    uint32_t   applied_depth_ = kNoClipApplied;
};

// ---------------------------------------------------------------------------
// IRenderer2D 适配器工厂
//
// 实现见 ui_render_bridge_rd.cpp（**只编入 GryceRenderer**：要 include
// render/render2d.h 与 rhi_handle.h，单元测试用 MockRenderer 不碰这条路径，
// 所以这里只前置声明、不把渲染器头文件拖进模型层）。
//
// 返回的适配器不持有 renderer 的所有权：renderer 必须在桥的使用期内有效。
// 无状态，可以每次绘制现建现弃（内置轨就是这么做的）。
// ---------------------------------------------------------------------------
// 把 drawlist 渲染到引擎的 IRenderer2D 上（引擎内唯一接线点）。
// scale = UI 逻辑像素 → 屏幕像素的倍数（HiDPI）；坐标、字号、裁剪框一起乘。
std::unique_ptr<UiRenderer> make_renderer2d_ui_renderer(
    gryce_engine::render::IRenderer2D* renderer, float scale = 1.0f);

} // namespace gryce_ui

#endif // GRYCE_UI_RENDER_BRIDGE_H
