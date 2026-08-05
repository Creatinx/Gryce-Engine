#include "fluent_window.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#include <imgui.h>

namespace gryce_engine::editor {

namespace {

GLFWwindow* g_Window = nullptr;
HWND g_Hwnd = nullptr;
float g_TitleBarHeight = 32.0f;
std::function<void()> g_MenuBarHook;

} // namespace

void FluentWindow_Init(GLFWwindow* window) {
    g_Window = window;
    g_Hwnd = glfwGetWin32Window(window);

    // 深色模式标题栏
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_Hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Acrylic 亚克力背景
    int backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(g_Hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    // 圆角
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_Hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 获取系统标题栏高度
    UINT dpi = GetDpiForWindow(g_Hwnd);
    g_TitleBarHeight = (32.0f * dpi) / 96.0f;
}

void FluentWindow_Shutdown() {
}

float FluentWindow_GetTitleBarHeight() {
    return g_TitleBarHeight;
}

bool FluentWindow_IsMaximized(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);
    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(hwnd, &wp);
    return wp.showCmd == SW_SHOWMAXIMIZED;
}

void FluentWindow_SetMenuBarHook(std::function<void()> hook) {
    g_MenuBarHook = std::move(hook);
}

void FluentWindow_RenderTitleBar(GLFWwindow* window) {
    if (!g_MenuBarHook) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float vx = viewport->Pos.x;
    float vy = viewport->Pos.y;
    float vw = viewport->Size.x;

    // 菜单栏放在客户区顶部，紧跟系统标题栏下方
    // panel_manager 已根据 GetTitleBarHeight() 偏移 DockSpace，
    // 所以这里菜单栏也需要占同样高度
    float menuH = g_TitleBarHeight;

    ImGui::SetNextWindowPos(ImVec2(vx, vy));
    ImGui::SetNextWindowSize(ImVec2(vw, menuH));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                              ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_MenuBar;

    // 增大 FramePadding 使菜单项填满菜单栏高度
    float fontHeight = ImGui::GetTextLineHeight();
    float framePadY = (menuH - fontHeight) * 0.5f - 1.0f;
    if (framePadY < 2.0f) framePadY = 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, framePadY));

    // 直接修改全局 style，确保 popup 在任意帧打开时都能读到正确的 padding
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 oldWindowPadding = style.WindowPadding;
    ImVec2 oldItemSpacing   = style.ItemSpacing;
    float  oldPopupRounding = style.PopupRounding;
    float  oldPopupBorderSize = style.PopupBorderSize;

    style.WindowPadding   = ImVec2(14.0f, 8.0f);
    style.ItemSpacing     = ImVec2(10.0f, 6.0f);
    style.PopupRounding   = 6.0f;
    style.PopupBorderSize = 1.0f;

    if (ImGui::Begin("##FluentMenuBar", nullptr, flags)) {
        if (ImGui::BeginMenuBar()) {
            g_MenuBarHook();
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    style.WindowPadding   = oldWindowPadding;
    style.ItemSpacing     = oldItemSpacing;
    style.PopupRounding   = oldPopupRounding;
    style.PopupBorderSize = oldPopupBorderSize;

    ImGui::PopStyleVar(4);
}

} // namespace gryce_engine::editor
