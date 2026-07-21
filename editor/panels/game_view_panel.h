#pragma once

#include "../editor_panel.h"

#include "math/math.h"

namespace gryce_engine {
namespace render { class RenderPipeline; class IImGuiBackend; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// GameViewPanel — 游戏视图面板（M1-E4）
// 显示主摄像机视角的运行时画面，与 Scene View 独立渲染目标。
// ---------------------------------------------------------------------------
class GameViewPanel : public EditorPanel {
public:
    GameViewPanel();

    void set_pipeline(render::RenderPipeline* pipeline) { pipeline_ = pipeline; }
    void set_imgui_backend(render::IImGuiBackend* backend) { backend_ = backend; }

    float content_width() const { return size_.x; }
    float content_height() const { return size_.y; }

protected:
    void on_imgui() override;

private:
    render::RenderPipeline* pipeline_ = nullptr;
    render::IImGuiBackend* backend_ = nullptr;
    ImVec2 size_ = ImVec2(0.0f, 0.0f);
};

} // namespace gryce_engine::editor
