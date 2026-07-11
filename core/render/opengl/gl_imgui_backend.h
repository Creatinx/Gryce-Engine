#pragma once

#include "render/imgui_backend.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLImGuiBackend — OpenGL3 渲染后端
// ---------------------------------------------------------------------------
class GLImGuiBackend : public IImGuiBackend {
public:
    ~GLImGuiBackend() override;

    bool init() override;
    void shutdown() override;
    void new_frame() override;
    void render_draw_data(ImDrawData* draw_data) override;

private:
    bool initialized_ = false;
};

} // namespace gryce_engine::render
