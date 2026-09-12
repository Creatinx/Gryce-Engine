// 启动画面子进程实现。
//
// 子进程以 --splash <marker> 启动，拥有独立 GLFW 窗口 / GL 上下文 / ImGui 上下文，
// 与主编辑器进程级隔离，展示品牌加载动画直到 marker 文件出现（主编辑器 init 完成）。

#include "splash_process.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <climits>
#include <cstdlib>
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "utils/glog/glog_lib.h"

namespace fs = std::filesystem;

namespace gryce_engine::editor {

namespace {

// 当前可执行文件绝对路径。
std::string current_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return {};
    return fs::path(std::wstring(buf, n)).string();
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
#endif
}

// 品牌加载画面：深色背景 + Logo 方块(G) + 标题 + 旋转圆弧 + "Loading..."。
void draw_splash_view(float t, int fbw, int fbh) {
    const ImVec2 win(static_cast<float>(fbw), static_cast<float>(fbh));
    const ImVec2 c(win.x * 0.5f, win.y * 0.5f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), win, IM_COL32(0x1E, 0x1E, 0x1E, 255));

    // Logo 方块 + G
    const float logo_s = 52.0f;
    const ImVec2 logo_p0(c.x - logo_s * 0.5f, c.y - logo_s * 0.5f - 36.0f);
    dl->AddRectFilled(logo_p0, ImVec2(logo_p0.x + logo_s, logo_p0.y + logo_s),
                      IM_COL32(0x4C, 0x9A, 0xFF, 255), 12.0f);
    dl->AddText(ImGui::GetFont(), 38.0f, ImVec2(c.x - 13.0f, logo_p0.y + 3.0f),
                IM_COL32(255, 255, 255, 255), "G");

    // 标题 + 副标题
    dl->AddText(ImGui::GetFont(), 19.0f, ImVec2(c.x - 66.0f, logo_p0.y + 72.0f),
                IM_COL32(0xE8, 0xEA, 0xED, 255), "Gryce Engine");
    dl->AddText(ImGui::GetFont(), 13.0f, ImVec2(c.x - 56.0f, logo_p0.y + 104.0f),
                IM_COL32(0x8A, 0x90, 0x99, 255), "Editor");

    // 旋转圆弧（加载指示器）
    const ImVec2 arc_c(c.x, c.y + 116.0f);
    const float r = 16.0f;
    const float a0 = t * 6.28318f;
    const float a1 = a0 + 4.2f; // 约 240°
    const int seg = 24;
    for (int i = 0; i < seg; ++i) {
        const float th0 = a0 + (a1 - a0) * float(i) / float(seg);
        const float th1 = a0 + (a1 - a0) * float(i + 1) / float(seg);
        const ImU32 col =
            IM_COL32(0x6C, 0xAF, 0xFF, int(255 * (0.25f + 0.75f * float(i) / seg)));
        dl->AddLine(ImVec2(arc_c.x + std::cos(th0) * r, arc_c.y + std::sin(th0) * r),
                    ImVec2(arc_c.x + std::cos(th1) * r, arc_c.y + std::sin(th1) * r),
                    col, 3.0f);
    }
    dl->AddText(ImGui::GetFont(), 13.0f, ImVec2(c.x - 48.0f, arc_c.y + 30.0f),
                IM_COL32(0x9A, 0xA0, 0xA8, 255), "Loading...");
}

} // namespace

// --splash 子进程入口：单线程、独立进程，展示加载窗口直到 marker 出现。
int run_splash_process(const char* marker_path) {
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    const int kW = 480, kH = 280;
    GLFWwindow* win = glfwCreateWindow(kW, kH, "Gryce Engine", nullptr, nullptr);
    if (!win) {
        GLOG_ERROR("SplashProcess: failed to create splash window");
        glfwTerminate();
        return 1;
    }

    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        glfwSetWindowPos(win, mx + (mw - kW) / 2, my + (mh - kH) / 2);
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    // 独立进程，唯一的 ImGui 上下文，无共享全局状态冲突。
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/consola.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    ImFont* font = nullptr;
    for (const char* path : candidates) {
        font = io.Fonts->AddFontFromFileTTF(path, 20.0f, nullptr,
                                            io.Fonts->GetGlyphRangesDefault());
        if (font) break;
    }
    if (!font) font = io.Fonts->AddFontDefault();
    ImGui_ImplOpenGL3_CreateDeviceObjects();

    const double start = glfwGetTime();
    while (!glfwWindowShouldClose(win)) {
        // marker 出现即主编辑器 init 完成，退出子进程。
        if (marker_path && fs::exists(fs::u8path(marker_path))) break;

        const double now = glfwGetTime();
        const float t = static_cast<float>(now - start);

        glfwPollEvents();

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        if (fbw == 0 || fbh == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw_splash_view(t, fbw, fbh);
        ImGui::Render();

        glViewport(0, 0, fbw, fbh);
        glClearColor(0.11f, 0.11f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}

SplashProcess::SplashProcess() {
    // 生成唯一 marker 路径（相对当前目录即可，子进程继承父进程 cwd）。
#ifdef _WIN32
    const std::string pid = std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
#else
    const std::string pid = std::to_string(static_cast<long>(getpid()));
#endif
    marker_path_ = "gryce_splash_" + pid + ".done";

    const std::string exe = current_exe_path();
    if (exe.empty()) {
        GLOG_ERROR("SplashProcess: failed to locate current executable");
        return;
    }

#ifdef _WIN32
    std::wstring wmarker = fs::u8path(marker_path_).wstring();
    std::wstring wcmd = L"\"" + fs::u8path(exe).wstring() + L"\" --splash \"" + wmarker + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(fs::u8path(exe).wstring().c_str(), wcmd.data(), nullptr, nullptr,
                       FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        handle_ = pi.hProcess;
    } else {
        GLOG_ERROR("SplashProcess: CreateProcessW failed (error {})",
                   static_cast<int>(GetLastError()));
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl(exe.c_str(), "GryceEditor", "--splash", marker_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    } else if (pid > 0) {
        handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    } else {
        GLOG_ERROR("SplashProcess: fork failed");
    }
#endif
}

SplashProcess::~SplashProcess() {
    stop();
}

void SplashProcess::stop() {
    if (!handle_) return;

    // 写 marker 通知子进程退出。
    if (!marker_path_.empty()) {
        std::ofstream f(fs::u8path(marker_path_), std::ios::out | std::ios::trunc);
        if (f) f << "done" << std::endl;
    }

#ifdef _WIN32
    WaitForSingleObject(handle_, INFINITE);
    CloseHandle(handle_);
#else
    int status = 0;
    pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(handle_));
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
#endif
    handle_ = nullptr;

    // 清理临时 marker 文件。
    if (!marker_path_.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(marker_path_), ec);
    }
}

} // namespace gryce_engine::editor