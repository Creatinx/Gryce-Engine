#include "gl_imgui_backend.h"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

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

} // namespace gryce_engine::render
