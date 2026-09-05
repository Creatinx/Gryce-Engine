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

    // OpenGL 端 ImTextureID 即 GLuint，直接返回纹理对象 id
    uint64_t imgui_texture_id(ITexture* texture) const override;

    // 重新创建 ImGui 设备对象（含字体纹理），用于运行时字体大小热重载。
    void rebuild_fonts() override;

private:
    bool initialized_ = false;
};

} // namespace gryce_engine::render
