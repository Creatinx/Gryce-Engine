#include "gl_imgui_backend.h"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include "render/opengl/gl_texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

GLImGuiBackend::~GLImGuiBackend() {
    shutdown();
}

bool GLImGuiBackend::init() {
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        GLOG_ERROR("GLImGuiBackend: ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    // 预创建设备对象，避免后续在 GL context 切换后创建失败
    ImGui_ImplOpenGL3_CreateDeviceObjects();
    initialized_ = true;
    GLOG_INFO("GLImGuiBackend initialized");
    return true;
}

void GLImGuiBackend::shutdown() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    initialized_ = false;
}

void GLImGuiBackend::new_frame() {
    ImGui_ImplOpenGL3_NewFrame();
}

void GLImGuiBackend::render_draw_data(ImDrawData* draw_data) {
    if (draw_data) {
        ImGui_ImplOpenGL3_RenderDrawData(draw_data);
    }
}

uint64_t GLImGuiBackend::imgui_texture_id(ITexture* texture) const {
    // OpenGL 端 ImTextureID 语义即 GLuint 纹理对象 id；
    // 纹理 id 创建后不可变，主线程读取安全。
    auto* gl_texture = dynamic_cast<GLTexture*>(texture);
    if (!gl_texture || !gl_texture->is_valid()) return 0;
    return static_cast<uint64_t>(gl_texture->texture_id());
}

} // namespace gryce_engine::render
