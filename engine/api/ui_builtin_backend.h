// ===========================================================================
// engine/api/ui_builtin_backend.h — 内置 UI 轨的运行时（引擎侧装配点）
//
// 分工：ui/ui_* 是纯模型层（无窗口、无渲染器、无输入，可单测），本文件是
// 唯一把它们接到引擎上的地方——拉输入、喂主题与度量、跑宿主回调、产出
// drawlist、再作为一条渲染命令提交出去。
//
// 一帧的时序（全部在 g_renderer.mutex 已持有的路径上，见 render_api.cpp）：
//
//   begin_build()                 拉输入 → 开 UI 帧（节流命中时返回 false）
//     ↳ 宿主回调（GUi_* 在这一段里提交控件）
//   end_build()                  收帧（回收未提交控件）→ 产出 drawlist
//   submit(ctx, renderer2d)      把 drawlist 打成一条命令推进本帧命令缓冲
//
// 为什么画在命令里而不是 render_2d_overlay 里：宿主一帧可能调多次
// GRender_RenderWorld / RenderGameView（编辑器同时画场景 + 游戏视图），
// 覆盖层 pass 会被跟着跑多次；而 UI 命令跟 ImGui 的 draw data 一样只在帧尾
// 执行一次，天然盖在最终画面上、也被截图包含。
//
// 两条轨的开关是进程级的（set_ui_backend）：0 = ImGui（默认，行为不变），
// 1 = 内置。切到内置时引擎不建 ImGui 上下文，GUi_* 全部走本运行时。
// ===========================================================================

#ifndef GRYCE_UI_BUILTIN_BACKEND_H
#define GRYCE_UI_BUILTIN_BACKEND_H

#include <cstdint>
#include <memory>

#include "api/ui_builtin_ops.h"
#include "ui/ui_builtin.h"
#include "ui/ui_render_bridge.h"

namespace gryce_engine::render {
class IRenderer2D;
} // namespace gryce_engine::render

namespace gryce_ui {

// 后端选择见 ui_builtin_ops.h（set_ui_backend / ui_backend / builtin_ui）。

// 把内置轨的实现表装进 ui_api.cpp（幂等）。渲染层在 GRender_Init 里调一次；
// 不装表时 GUi_* 一律走 ImGui 分支。
void install_builtin_ui();

// ---------------------------------------------------------------------------
// BuiltinRuntime — 进程内唯一实例
// ---------------------------------------------------------------------------
class BuiltinRuntime {
public:
    static BuiltinRuntime& instance();

    // 视口尺寸（像素）与文本度量来源。每帧开头由渲染层喂。
    // renderer 为空时退回估算度量（ui_approx_text_width）——单测与无字体
    // 环境靠这条路径，行为是"文字能摆、宽度略偏"。
    void set_frame_context(int width, int height, gryce_engine::render::IRenderer2D* renderer);

    // 节流：与 ImGui 轨同一套规则（frame % interval == 1 时重建；首帧必建）。
    void set_throttle(int interval, uint64_t frame);

    // HiDPI：UI 的逻辑像素 → 屏幕像素的倍数。模型层始终在逻辑坐标里布局，
    // 鼠标在进入模型层前除以它，几何在渲染桥里乘回去（含字号）。
    void  set_scale(float s) { scale_ = (s > 0.0f) ? s : 1.0f; }
    float scale() const { return scale_; }

    // 帧构建。返 false = 本帧不重建（节流命中或没有回调），沿用上次的 drawlist。
    bool begin_build();
    void end_build();

    // 宿主回调执行中（GUi_* 的可用判据之一）。
    bool building() const { return building_; }

    // 把最近一次构建结果直接画进 2D 覆盖层（无帧可画时是空操作）。
    //
    // **必须在主线程调**（g_renderer.mutex 已持有的路径上）：Renderer2D 的
    // 成员（相机、顶点批次、屏幕尺寸）只由主线程写，渲染线程只执行它压进去的
    // 命令。把这段打包成渲染命令去渲染线程跑会与下一帧的主线程写竞争
    //（症状是随机的堆损坏，不是稳定的崩溃）。
    void draw_now(gryce_engine::render::IRenderer2D* renderer);

    // ---- 宿主可见状态 ----
    BuiltinUi& ui() { return ui_; }
    const BuiltinUi& ui() const { return ui_; }
    float display_width() const { return width_; }
    float display_height() const { return height_; }
    float text_width(const char* text) const;   // 与绘制同一口径
    float line_height() const;
    float frame_height() const;
    int   last_draw_commands() const { return last_draws_; }

    // 内容变化计数：宿主可以拿它判断"这一帧重建了没有"。
    uint64_t build_count() const { return build_count_; }

    void reset();   // 关掉/重开引擎时清状态（悬空的控件 id 不该跨引擎实例）

private:
    BuiltinRuntime() = default;

    BuiltinUi        ui_;
    UiDrawList       list_;          // 最近一次构建的命令缓冲
    bool             has_frame_ = false;

    // 文本度量：BuiltinUi 只存指针（布局与绘制共用一份口径），所以它的生命
    // 周期必须覆盖整帧——每次 begin_build 重造，end_build 之后才作废。
    std::unique_ptr<UiTextMetrics> metrics_;

    int              width_  = 0;
    int              height_ = 0;
    gryce_engine::render::IRenderer2D* renderer_ = nullptr;

    int              interval_ = 1;
    uint64_t         frame_    = 0;
    uint64_t         build_count_ = 0;
    int              last_draws_  = 0;
    float            scale_    = 1.0f;

    bool             building_ = false;
    bool             rebuild_due_ = true;
};

} // namespace gryce_ui

#endif // GRYCE_UI_BUILTIN_BACKEND_H
