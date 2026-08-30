// GryceLauncher — 项目启动器
// 新建 / 打开项目，然后拉起 GryceEditor 并退出自身。

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <unistd.h>
#include <climits>
#endif

#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <nlohmann/json.hpp>

#include "platform/window.h"
#include "render/imgui_backend.h"
#include "render/opengl/imgui_renderer.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

#include "blender_theme.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace gryce_engine::editor {

namespace {

std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buffer).parent_path();
    }
#else
    char buffer[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        return fs::path(buffer).parent_path();
    }
#endif
    return fs::current_path();
}

std::filesystem::path config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0]) {
        return fs::path(appdata) / "GryceEngine" / "launcher.json";
    }
#endif
    return executable_dir() / "launcher.json";
}

std::vector<std::string> load_recent() {
    std::vector<std::string> out;
    std::ifstream in(config_path());
    if (!in.is_open()) return out;
    try {
        json j = json::parse(in);
        if (j.contains("recent") && j["recent"].is_array()) {
            for (const auto& item : j["recent"]) {
                if (item.is_string()) out.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

void save_recent(const std::vector<std::string>& recent) {
    json j;
    j["recent"] = recent;
    std::error_code ec;
    fs::create_directories(config_path().parent_path(), ec);
    std::ofstream out(config_path());
    if (out.is_open()) {
        out << j.dump(2);
    }
}

void push_recent(std::vector<std::string>& recent, const std::string& path) {
    auto it = std::find(recent.begin(), recent.end(), path);
    if (it != recent.end()) recent.erase(it);
    recent.insert(recent.begin(), path);
    if (recent.size() > 12) recent.resize(12);
    save_recent(recent);
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string pick_folder() {
    std::string result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return result;

    IFileDialog* dialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr)) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = wide_to_utf8(path ? path : L"");
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    CoUninitialize();
    return result;
}
#else
std::string pick_folder() {
    return {};
}
#endif

// 简单的文件夹图标（与文件资源管理器风格一致，DrawList 绘制）
void draw_folder_icon(ImDrawList* dl, ImVec2 pos, float size, ImU32 accent) {
    const float w = size;
    const float h = size * 0.82f;
    const ImU32 body = IM_COL32(0x5B, 0x7F, 0xA6, 255);
    const ImU32 body_dark = IM_COL32(0x4A, 0x68, 0x8B, 255);
    const ImU32 tab = IM_COL32(0x7A, 0x9C, 0xC4, 255);

    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.22f),
                      ImVec2(pos.x + w * 0.38f, pos.y + h * 0.42f), tab, 1.0f);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.30f),
                      ImVec2(pos.x + w, pos.y + h), body, 2.0f);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.84f),
                      ImVec2(pos.x + w, pos.y + h), body_dark, 2.0f);
    (void)accent;
}

// 收藏星标（两枚交错三角近似六角星）
void draw_star(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddTriangleFilled(ImVec2(c.x, c.y - r),
                          ImVec2(c.x - r * 0.9f, c.y + r * 0.9f),
                          ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), color);
    dl->AddTriangleFilled(ImVec2(c.x, c.y + r),
                          ImVec2(c.x - r * 0.9f, c.y - r * 0.9f),
                          ImVec2(c.x + r * 0.9f, c.y - r * 0.9f), color);
}

// 播放（运行）三角形
void draw_play_icon(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.7f, c.y - r),
                          ImVec2(c.x - r * 0.7f, c.y + r),
                          ImVec2(c.x + r, c.y), color);
}

// 下拉箭头
void draw_chevron(ImDrawList* dl, ImVec2 c, float s, ImU32 color) {
    dl->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.5f),
                          ImVec2(c.x + s, c.y - s * 0.5f),
                          ImVec2(c.x, c.y + s * 0.6f), color);
}

// 文件夹最后修改时间 -> "YYYY-MM-DD HH:MM"
std::string format_file_time(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return {};
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void open_in_explorer(const std::string& path) {
#ifdef _WIN32
    std::wstring wide = fs::u8path(path).wstring();
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

bool launch_editor(const std::string& project_path, std::string& error) {
    const fs::path editor_exe = executable_dir() / "GryceEditor.exe";
    if (!fs::exists(editor_exe)) {
        error = "GryceEditor.exe not found next to launcher: " +
                editor_exe.string();
        return false;
    }
#ifdef _WIN32
    std::wstring exe_w = editor_exe.wstring();
    std::wstring project_w = fs::u8path(project_path).wstring();
    const std::wstring cmd =
        L"\"" + exe_w + L"\" --project \"" + project_w + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe_w.c_str(), const_cast<wchar_t*>(cmd.c_str()),
                             nullptr, nullptr, FALSE,
                             CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, editor_exe.parent_path().wstring().c_str(),
                             &si, &pi);
    if (!ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "CreateProcess failed (code %lu)",
                      static_cast<unsigned long>(GetLastError()));
        error = buf;
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        execl(editor_exe.string().c_str(), editor_exe.string().c_str(),
              "--project", project_path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

void apply_launcher_font() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    constexpr float k_font_size = 20.0f;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;

    std::vector<fs::path> candidates = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
#endif
    };
    bool loaded = false;
    for (const auto& font : candidates) {
        if (!fs::exists(font)) continue;
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            font.string().c_str(), k_font_size, &cfg,
            io.Fonts->GetGlyphRangesChineseFull());
        loaded = true;
        break;
    }
    if (!loaded) {
        io.FontDefault = io.Fonts->AddFontDefault(&cfg);
    }

    ImGui::GetStyle().ScaleAllSizes(1.38f);
}

} // namespace

// ---------------------------------------------------------------------------
// LauncherApp — 项目启动器主程序
// ---------------------------------------------------------------------------
class LauncherApp {
public:
    bool init();
    int run();
    void shutdown();

private:
    void render_ui();
    void render_main_window();
    void render_new_project_dialog();
    bool create_new_project(std::string& error);
    void open_project(const std::string& path);

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::RenderContext> render_ctx_;
    std::unique_ptr<render::ImGuiRenderer> imgui_;

    std::vector<std::string> recent_;
    std::array<char, 256> project_name_{};
    std::array<char, 1024> project_parent_{};
    std::array<char, 128> filter_text_{};
    std::string status_;
    bool status_error_ = false;
    bool running_ = false;

    int selected_project_index_ = -1;
    int sort_mode_ = 0; // 0=最近编辑 1=名称 2=路径
    bool show_new_project_ = false;
};

bool LauncherApp::init() {
    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);
    utils::GLog::instance().set_logger(
        std::make_unique<utils::ConsoleLogger>());

    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    // 窗口尺寸
    int work_w = 1920, work_h = 1080;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorWorkarea(monitor, nullptr, nullptr, &work_w, &work_h);
    }
    const int win_w = std::clamp(static_cast<int>(work_w * 0.65f), 1000, 1400);
    const int win_h = std::clamp(static_cast<int>(work_h * 0.78f), 700, 900);
    window_ = std::make_unique<platform::Window>(
        "Gryce Engine - Project Manager", win_w, win_h,
        platform::WindowMode::Windowed, platform::WindowContextType::OpenGL);
    if (!window_ || !window_->is_valid()) {
        GLOG_ERROR("Failed to create launcher window");
        platform::Window::shutdown_sdk();
        return false;
    }

    render_ctx_ = std::make_unique<render::RenderContext>();
    auto backend = render::create_render_backend(render::RenderAPI::OpenGL);
    if (!backend ||
        !render_ctx_->init(window_->native_handle(), std::move(backend))) {
        GLOG_ERROR("Failed to initialize render context");
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }

    imgui_ = std::make_unique<render::ImGuiRenderer>();
    auto imgui_backend = render_ctx_->create_imgui_backend();
    if (!imgui_->init(window_->native_handle(), std::move(imgui_backend))) {
        GLOG_ERROR("Failed to initialize ImGui");
        render_ctx_->shutdown();
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }

    // 与编辑器保持同一套 Blender 主题
    StyleColorsBlender();
    apply_launcher_font();
    imgui_->backend()->rebuild_fonts();

    recent_ = load_recent();

    // 默认新建项目位置
#ifdef _WIN32
    if (const char* userprofile = std::getenv("USERPROFILE")) {
        std::string default_dir = std::string(userprofile) + "/Documents/GryceProjects";
        std::strncpy(project_parent_.data(), default_dir.c_str(),
                     project_parent_.size() - 1);
    }
#endif

    running_ = true;
    GLOG_INFO("Launcher initialized successfully");
    return true;
}

void LauncherApp::shutdown() {
    if (!window_) return;
    running_ = false;
    if (imgui_) imgui_->shutdown();
    if (render_ctx_) render_ctx_->shutdown();
    window_.reset();
    platform::Window::shutdown_sdk();
}

int LauncherApp::run() {
    if (!running_ || !window_) return -1;

    while (running_ && window_ && !window_->should_close()) {
        window_->poll_events();
        if (window_->get_key(GLFW_KEY_ESCAPE)) {
            window_->request_close();
            break;
        }

        imgui_->begin_frame();
        render_ui();
        imgui_->end_frame([this](ImDrawData* draw_data,
                                 std::shared_ptr<std::promise<void>> sync_promise) {
            auto owned = imgui_->clone_draw_data(draw_data);
            render_ctx_->push_command(
                [owned, sync_promise, this](render::IRenderBackend*) {
                    imgui_->render_draw_data(owned.get());
                    sync_promise->set_value();
                });
        });
        render_ctx_->present_sync();
    }
    return 0;
}

void LauncherApp::render_ui() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

    render_main_window();

    if (show_new_project_) {
        render_new_project_dialog();
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

void LauncherApp::render_main_window() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const ImVec2 win_size = ImGui::GetContentRegionAvail();
    const float width = win_size.x;
    const float height = win_size.y;

    // 与编辑器 Blender 主题一致的取色
    const ImU32 col_header =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    const ImU32 col_panel =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImU32 col_border =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Border));
    const ImU32 col_text =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const ImU32 col_text_dim =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const ImU32 col_accent = IM_COL32(0x4C, 0x9A, 0xFF, 255);
    const ImU32 col_accent_dim = IM_COL32(0x4C, 0x9A, 0xFF, 40);

    const float top_h = 52.0f;
    const float toolbar_h = 56.0f;
    const float status_h = 30.0f;
    const float sidebar_w = 176.0f;
    const float content_y = top_h + toolbar_h;
    const float content_h = height - top_h - toolbar_h - status_h;

    const ImU32 col_card = IM_COL32(0x2B, 0x2B, 0x2B, 255);
    const ImU32 col_card_b = IM_COL32(0x3A, 0x3A, 0x3A, 255);

    // ===== 顶栏：logo + 项目/资源商店 tab + 设置 =====
    dl->AddRectFilled(win_pos, win_pos + ImVec2(width, top_h), col_header);
    dl->AddRectFilled(win_pos + ImVec2(0, top_h - 1),
                      win_pos + ImVec2(width, top_h), col_border);

    // Logo + 标题
    const float logo_x = 16.0f;
    const float logo_y = 16.0f;
    dl->AddRectFilled(win_pos + ImVec2(logo_x, logo_y),
                      win_pos + ImVec2(logo_x + 32, logo_y + 32), col_accent, 6.0f);
    dl->AddText(win_pos + ImVec2(logo_x + 9.0f, logo_y + 5.0f),
                IM_COL32(255, 255, 255, 255), "G");
    dl->AddText(win_pos + ImVec2(logo_x + 42.0f, logo_y + 1.0f),
                col_text, "Gryce Engine");
    dl->AddText(win_pos + ImVec2(logo_x + 42.0f, logo_y + 20.0f),
                col_text_dim, "Project Manager");

    // 顶部 tab：项目（激活）/ 资源商店（置灰）
    const float tab_y = top_h * 0.5f - 10.0f;
    const float tab_cx = width * 0.5f;
    draw_chevron(dl, ImVec2(tab_cx - 132.0f, tab_y + 10.0f), 6.0f, col_accent);
    dl->AddText(win_pos + ImVec2(tab_cx - 118.0f, tab_y), col_text, "项目");
    dl->AddText(win_pos + ImVec2(tab_cx + 10.0f, tab_y), col_text_dim, "资源商店");
    dl->AddRectFilled(win_pos + ImVec2(tab_cx - 128.0f, top_h - 3.0f),
                      win_pos + ImVec2(tab_cx - 78.0f, top_h), col_accent);

    // 右侧：设置
    const ImVec2 btn_size(96.0f, 36.0f);
    ImGui::SetCursorPos(ImVec2(width - btn_size.x - 16.0f, (top_h - btn_size.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("设置", btn_size)) {
        status_ = "设置面板尚未实现";
        status_error_ = false;
    }
    ImGui::PopStyleColor(3);

    // ===== 工具条：新建/导入 + 搜索 + 排序 =====
    const float toolbar_y = top_h;
    dl->AddRectFilled(win_pos + ImVec2(0, toolbar_y),
                      win_pos + ImVec2(width, toolbar_y + toolbar_h), col_panel);
    dl->AddRectFilled(win_pos + ImVec2(0, toolbar_y + toolbar_h - 1),
                      win_pos + ImVec2(width, toolbar_y + toolbar_h), col_border);

    const float tool_btn_h = 38.0f;
    ImGui::SetCursorPos(ImVec2(12.0f, toolbar_y + (toolbar_h - tool_btn_h) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.60f, 1.00f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 1.00f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.50f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button("+ 新建项目", ImVec2(110.0f, tool_btn_h))) {
        show_new_project_ = true;
    }
    ImGui::PopStyleColor(4);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("+ 导入", ImVec2(90.0f, tool_btn_h))) {
        const std::string folder = pick_folder();
        if (!folder.empty()) open_project(folder);
    }
    ImGui::PopStyleColor(3);

    // 搜索框
    ImGui::SameLine();
    const float search_w = width * 0.34f;
    ImGui::SetNextItemWidth(search_w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col_card);
    ImGui::InputTextWithHint("##filter", "搜索项目...", filter_text_.data(),
                             filter_text_.size());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // 排序下拉
    ImGui::SameLine(width - 210.0f);
    const char* sort_labels[] = {"最近编辑", "名称", "路径"};
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##sort", sort_labels[sort_mode_],
                          ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(sort_labels[i], i == sort_mode_)) {
                sort_mode_ = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(3);

    // ===== 主区：项目卡片列表 =====
    const float list_w = width - sidebar_w;

    ImGui::SetCursorPos(ImVec2(0, content_y));
    ImGui::BeginChild("project_list_panel", ImVec2(list_w, content_h), false);

    ImGui::BeginChild("project_list", ImVec2(list_w, content_h), false);

    std::string filter(filter_text_.data());
    filter.erase(std::find(filter.begin(), filter.end(), '\0'), filter.end());
    for (auto& c : filter) c = std::tolower(c);

    std::vector<int> indices;
    for (int i = 0; i < (int)recent_.size(); ++i) {
        fs::path p(recent_[i]);
        std::string name = p.filename().string();
        std::string path = recent_[i];
        for (auto& c : name) c = std::tolower(c);
        for (auto& c : path) c = std::tolower(c);
        if (!filter.empty() && name.find(filter) == std::string::npos &&
            path.find(filter) == std::string::npos) {
            continue;
        }
        indices.push_back(i);
    }

    // 排序：名称 / 路径（最近编辑保持原序）
    if (sort_mode_ == 1) {
        std::sort(indices.begin(), indices.end(), [this](int a, int b) {
            return fs::path(recent_[a]).filename().string() <
                   fs::path(recent_[b]).filename().string();
        });
    } else if (sort_mode_ == 2) {
        std::sort(indices.begin(), indices.end(),
                  [this](int a, int b) { return recent_[a] < recent_[b]; });
    }

    if (indices.empty()) {
        const float cx = list_w * 0.5f;
        ImGui::SetCursorPos(ImVec2(cx - 90.0f, content_h * 0.36f));
        ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
        ImGui::TextUnformatted("还没有项目");
        ImGui::SetCursorPos(ImVec2(cx - 130.0f, content_h * 0.36f + 26.0f));
        ImGui::TextUnformatted("点击左上角“+ 新建项目”开始");
        ImGui::PopStyleColor();
    } else {
        // 项目卡片：星标 + 缩略图 + 名称/路径 + 版本/时间
        std::string open_path;
        const float card_h = 92.0f;
        ImDrawList* cdl = ImGui::GetWindowDrawList(); // 子窗口 draw list，避免被面板背景遮挡
        for (int idx : indices) {
            const fs::path p(recent_[idx]);
            const std::string name = p.filename().string();
            const std::string path = recent_[idx];
            const std::string date = format_file_time(p);

            ImGui::PushID(idx);
            const bool is_selected = (idx == selected_project_index_);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("##card", is_selected,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(list_w - 24.0f, card_h))) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    open_path = recent_[idx];
                } else {
                    selected_project_index_ = idx;
                }
            }

            const ImVec2 p1(p0.x + list_w - 24.0f, p0.y + card_h);
            cdl->AddRectFilled(p0, p1,
                               is_selected ? col_accent_dim : col_card, 6.0f);
            cdl->AddRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1),
                         is_selected ? col_accent : col_card_b, 6.0f);

            // 收藏星标
            draw_star(cdl, ImVec2(p0.x + 26.0f, p0.y + card_h * 0.5f), 11.0f,
                      is_selected ? col_accent : col_text_dim);

            // 项目缩略图（圆角块 + 蓝底 G）
            const ImVec2 thumb(p0.x + 48.0f, p0.y + 16.0f);
            cdl->AddRectFilled(thumb, thumb + ImVec2(60, 60), col_card_b, 10.0f);
            cdl->AddRectFilled(thumb + ImVec2(16, 16), thumb + ImVec2(44, 44),
                               col_accent, 8.0f);
            cdl->AddText(thumb + ImVec2(23, 15), IM_COL32(255, 255, 255, 255), "G");

            // 名称 + 路径
            cdl->AddText(ImVec2(p0.x + 124.0f, p0.y + 20.0f), col_text, name.c_str());
            draw_folder_icon(cdl, ImVec2(p0.x + 124.0f, p0.y + 52.0f), 18.0f,
                             col_text_dim);
            cdl->AddText(ImVec2(p0.x + 148.0f, p0.y + 52.0f), col_text_dim,
                         path.c_str());

            // 右侧：版本 + 最近编辑
            cdl->AddText(ImVec2(p1.x - 210.0f, p0.y + 30.0f), col_text_dim, "v0.1.0");
            cdl->AddText(ImVec2(p1.x - 130.0f, p0.y + 30.0f), col_text_dim,
                         date.c_str());

            ImGui::PopID();

            if (!open_path.empty()) {
                open_project(open_path);
                break;
            }
        }
    }

    ImGui::EndChild();
    ImGui::EndChild(); // project_list_panel

    // ===== 右侧：竖排操作按钮 =====
    ImGui::SetCursorPos(ImVec2(list_w, content_y));
    dl->AddRectFilled(win_pos + ImVec2(list_w, content_y),
                      win_pos + ImVec2(width, content_y + content_h), col_panel);
    dl->AddRectFilled(win_pos + ImVec2(list_w, content_y),
                      win_pos + ImVec2(list_w + 1, content_y + content_h), col_border);
    ImGui::BeginChild("side_panel", ImVec2(sidebar_w, content_h), false);
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    sdl->AddRectFilled(win_pos + ImVec2(list_w, content_y),
                       win_pos + ImVec2(list_w + 1, content_y + content_h), col_border);

    const bool has_selection = selected_project_index_ >= 0 &&
                               selected_project_index_ < (int)recent_.size();
    const float btn_w = sidebar_w - 32.0f;
    ImGui::BeginDisabled(!has_selection);

    // 编辑项目（主按钮 + 下拉箭头）
    ImGui::SetCursorPos(ImVec2(16.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.60f, 1.00f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 1.00f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.50f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button("编辑项目", ImVec2(btn_w - 40.0f, 44.0f))) {
        open_project(recent_[selected_project_index_]);
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.20f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.28f, 0.45f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button("▼", ImVec2(40.0f, 44.0f))) {
        ImGui::OpenPopup("edit_menu");
    }
    ImGui::PopStyleColor(3);
    if (ImGui::BeginPopup("edit_menu")) {
        if (ImGui::MenuItem("打开目录")) {
            open_in_explorer(recent_[selected_project_index_]);
        }
        if (ImGui::MenuItem("从列表移除")) {
            recent_.erase(recent_.begin() + selected_project_index_);
            save_recent(recent_);
            selected_project_index_ = -1;
            status_ = "已从列表移除";
            status_error_ = false;
        }
        ImGui::EndPopup();
    }

    // 运行（占位，未实现）
    ImGui::SetCursorPos(ImVec2(16.0f, 72.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    if (ImGui::Button("运行", ImVec2(btn_w, 44.0f))) {
        status_ = "运行功能尚未实现";
        status_error_ = true;
    }
    ImGui::PopStyleColor(3);

    // 打开目录
    ImGui::SetCursorPos(ImVec2(16.0f, 124.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("打开目录", ImVec2(btn_w, 44.0f))) {
        open_in_explorer(recent_[selected_project_index_]);
    }
    ImGui::PopStyleColor(3);

    // 重命名（占位）
    ImGui::SetCursorPos(ImVec2(16.0f, 176.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    if (ImGui::Button("重命名", ImVec2(btn_w, 44.0f))) {
        status_ = "重命名尚未实现";
        status_error_ = true;
    }
    ImGui::PopStyleColor(3);

    // 从列表移除
    ImGui::SetCursorPos(ImVec2(16.0f, 228.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.75f, 1.0f));
    if (ImGui::Button("从列表移除", ImVec2(btn_w, 44.0f))) {
        recent_.erase(recent_.begin() + selected_project_index_);
        save_recent(recent_);
        selected_project_index_ = -1;
        status_ = "已从列表移除";
        status_error_ = false;
    }
    ImGui::PopStyleColor(3);

    ImGui::EndDisabled();
    ImGui::EndChild(); // side_panel

    // ===== 状态栏 =====
    const float status_y = content_y + content_h;
    dl->AddRectFilled(win_pos + ImVec2(0, status_y),
                      win_pos + ImVec2(width, height), col_header);
    dl->AddRectFilled(win_pos + ImVec2(0, status_y),
                      win_pos + ImVec2(width, status_y + 1), col_border);

    char count_buf[64];
    std::snprintf(count_buf, sizeof(count_buf), "%zu 个项目", recent_.size());
    ImGui::SetCursorPos(ImVec2(12.0f, status_y + 7.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    ImGui::TextUnformatted(count_buf);
    ImGui::PopStyleColor();

    if (!status_.empty()) {
        const float status_w = ImGui::CalcTextSize(status_.c_str()).x;
        ImGui::SetCursorPos(ImVec2(width * 0.5f - status_w * 0.5f, status_y + 7.0f));
        ImGui::TextColored(status_error_ ? ImVec4(0.85f, 0.35f, 0.30f, 1.0f)
                                          : ImVec4(0.30f, 0.75f, 0.45f, 1.0f),
                           "%s", status_.c_str());
    }

    ImGui::SetCursorPos(ImVec2(width - 72.0f, status_y + 7.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    ImGui::TextUnformatted("v0.1.0");
    ImGui::PopStyleColor();
}

void LauncherApp::render_new_project_dialog() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

    // 半透明遮罩（Godot 模态风格）
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(viewport->WorkPos,
                      viewport->WorkPos + viewport->WorkSize,
                      IM_COL32(0, 0, 0, 150));

    const ImVec2 dialog_size(540.0f, 360.0f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(dialog_size, ImGuiCond_Appearing);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    bool dialog_open = true;
    ImGui::Begin("##new_project", &dialog_open, flags);

    if (!dialog_open) {
        show_new_project_ = false;
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    // 标题 + 关闭按钮
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted("新建项目");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::SameLine(dialog_size.x - 52.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.25f, 0.25f, 0.45f));
    if (ImGui::SmallButton("x")) {
        show_new_project_ = false;
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 项目名称
    ImGui::TextDisabled("项目名称");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##project_name", project_name_.data(), project_name_.size());
    ImGui::Spacing();

    // 项目位置
    ImGui::TextDisabled("项目位置");
    ImGui::Spacing();
    const float browse_btn_w = 76.0f;
    ImGui::SetNextItemWidth(-browse_btn_w - 10.0f);
    ImGui::InputText("##project_parent", project_parent_.data(),
                     project_parent_.size());
    ImGui::SameLine();
    if (ImGui::Button("浏览", ImVec2(browse_btn_w, 0.0f))) {
        const std::string folder = pick_folder();
        if (!folder.empty()) {
            std::strncpy(project_parent_.data(), folder.c_str(),
                         project_parent_.size() - 1);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("将在所选位置创建“项目名/scenes”目录");
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 底部按钮
    const float btn_w = 120.0f;
    ImGui::SetCursorPosX(dialog_size.x - 20.0f - btn_w * 2.0f - 12.0f);
    if (ImGui::Button("取消", ImVec2(btn_w, 44.0f))) {
        show_new_project_ = false;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.60f, 1.00f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 1.00f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.50f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button("创建", ImVec2(btn_w, 44.0f))) {
        std::string error;
        if (create_new_project(error)) {
            show_new_project_ = false;
            running_ = false;
        } else {
            status_ = error;
            status_error_ = true;
        }
    }
    ImGui::PopStyleColor(4);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

bool LauncherApp::create_new_project(std::string& error) {
    std::string name(project_name_.data());
    std::string parent(project_parent_.data());
    name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
    parent.erase(std::find(parent.begin(), parent.end(), '\0'), parent.end());

    if (name.empty()) {
        error = "项目名称不能为空";
        return false;
    }
    if (parent.empty()) {
        error = "项目位置不能为空";
        return false;
    }

    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        error = "目录不存在: " + parent;
        return false;
    }

    const fs::path project_dir = fs::path(parent) / name;
    if (fs::exists(project_dir, ec)) {
        error = "目录已存在: " + project_dir.string();
        return false;
    }

    if (!fs::create_directories(project_dir / "scenes", ec)) {
        error = "创建目录失败: " + ec.message();
        return false;
    }

    open_project(project_dir.string());
    return true;
}

void LauncherApp::open_project(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        status_ = "不是有效目录: " + path;
        status_error_ = true;
        return;
    }
    std::string error;
    if (!launch_editor(path, error)) {
        status_ = error;
        status_error_ = true;
        return;
    }
    push_recent(recent_, path);
    status_ = "已启动 " + path;
    status_error_ = false;
    // 保持启动器运行，不退出
}

} // namespace gryce_engine::editor

int run_launcher() {
    gryce_engine::editor::LauncherApp app;
    if (!app.init()) {
        return 1;
    }
    int result = app.run();
    app.shutdown();
    return result;
}
