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

#include "editor_theme.h"
#include "launcher_shared.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace gryce_engine::editor {

namespace {
using namespace launcher;

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

void open_in_explorer(const std::string& path) {
#ifdef _WIN32
    std::wstring wide = fs::u8path(path).wstring();
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

void apply_launcher_font() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    constexpr float k_font_size = 19.0f;

    // 等宽字体（兼顾界面一致性与数字/路径可读性），与编辑器同款偏好。
    const std::vector<fs::path> mono_candidates = {
#ifdef _WIN32
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
#endif
    };
    fs::path font_path;
    for (const auto& candidate : mono_candidates) {
        if (!fs::exists(candidate)) continue;
        font_path = candidate;
        break;
    }
    ImFontConfig mono_cfg;
    mono_cfg.OversampleH = 2;
    mono_cfg.OversampleV = 1;
    if (!font_path.empty()) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            font_path.string().c_str(), k_font_size, &mono_cfg,
            io.Fonts->GetGlyphRangesDefault());
    }
    if (!io.FontDefault) {
        io.FontDefault = io.Fonts->AddFontDefault(&mono_cfg);
    }

    // 等宽字体通常不含 CJK；合并系统中文字体，保证中文界面可读。
    const std::vector<fs::path> cjk_candidates = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
#endif
    };
    for (const auto& candidate : cjk_candidates) {
        if (!fs::exists(candidate)) continue;
        ImFontConfig cjk_cfg;
        cjk_cfg.MergeMode = true;
        cjk_cfg.OversampleH = 2;
        cjk_cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), k_font_size,
                                     &cjk_cfg,
                                     io.Fonts->GetGlyphRangesChineseFull());
        break;
    }

    ImGui::GetStyle().ScaleAllSizes(1.1f);
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
    void render_settings_dialog();
    bool create_new_project(std::string& error);
    void open_project(const std::string& path);

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::RenderContext> render_ctx_;
    std::unique_ptr<render::ImGuiRenderer> imgui_;

    launcher::LauncherSettings settings_;   // 从 launcher.json 载入的设置

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

    // 设置对话框编辑态
    bool show_settings_ = false;
    std::array<char, 1024> set_default_dir_{};
    std::array<char, 512> set_editor_args_{};
    std::array<char, 128> set_version_{};
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

    // 与编辑器保持同一套主题（Dark / Light），配色与编辑器完全一致
    ApplyEditorTheme(EditorTheme::Dark);
    apply_launcher_font();
    imgui_->backend()->rebuild_fonts();

    recent_ = load_recent();
    settings_ = launcher::load_settings();

    // 设置对话框初始值
    std::strncpy(set_default_dir_.data(), settings_.defaultDir.c_str(),
                 set_default_dir_.size() - 1);
    std::strncpy(set_editor_args_.data(), settings_.editorArgs.c_str(),
                 set_editor_args_.size() - 1);
    std::strncpy(set_version_.data(), settings_.lastVersion.c_str(),
                 set_version_.size() - 1);

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
    if (show_settings_) {
        render_settings_dialog();
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

    // 与编辑器主题一致的取色
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
    const ImU32 col_card = IM_COL32(0x2B, 0x2B, 0x2B, 255);
    const ImU32 col_card_b = IM_COL32(0x3A, 0x3A, 0x3A, 255);

    const float top_h = 48.0f;      // 顶栏（深色标题条）
    const float status_h = 26.0f;   // 底部状态栏
    const float rail_w = 224.0f;    // 左侧操作栏
    const float body_y = top_h;
    const float status_y = height - status_h;
    const float body_h = height - top_h - status_h;

    // ===== 顶栏：Gryce 品牌 + 右侧设置 =====
    dl->AddRectFilled(win_pos, win_pos + ImVec2(width, top_h), col_header);
    dl->AddRectFilled(win_pos + ImVec2(0, top_h - 1),
                      win_pos + ImVec2(width, top_h), col_border);

    const float logo_x = 14.0f, logo_y = 10.0f;
    dl->AddRectFilled(win_pos + ImVec2(logo_x, logo_y),
                      win_pos + ImVec2(logo_x + 28, logo_y + 28), col_accent, 6.0f);
    dl->AddText(win_pos + ImVec2(logo_x + 8.0f, logo_y + 4.0f),
                IM_COL32(255, 255, 255, 255), "G");
    dl->AddText(win_pos + ImVec2(logo_x + 40.0f, logo_y + 1.0f),
                col_text, "Gryce Engine");
    dl->AddText(win_pos + ImVec2(logo_x + 40.0f, logo_y + 17.0f),
                col_text_dim, "Project Manager");

    // 设置按钮（顶栏右侧）
    const ImVec2 top_btn(84.0f, 30.0f);
    ImGui::SetCursorPos(ImVec2(width - top_btn.x - 12.0f, (top_h - top_btn.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("设置", top_btn)) {
        std::strncpy(set_default_dir_.data(), settings_.defaultDir.c_str(),
                     set_default_dir_.size() - 1);
        std::strncpy(set_editor_args_.data(), settings_.editorArgs.c_str(),
                     set_editor_args_.size() - 1);
        std::strncpy(set_version_.data(), settings_.lastVersion.c_str(),
                     set_version_.size() - 1);
        show_settings_ = true;
    }
    ImGui::PopStyleColor(3);

    // ===== 左侧操作栏（VS Start 风格） =====
    ImGui::SetCursorPos(ImVec2(0, body_y));
    ImGui::BeginChild("left_rail", ImVec2(rail_w, body_h), false);
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    rdl->AddRectFilled(win_pos + ImVec2(0, body_y),
                       win_pos + ImVec2(rail_w, status_y), col_panel);
    rdl->AddRectFilled(win_pos + ImVec2(rail_w - 1, body_y),
                       win_pos + ImVec2(rail_w, status_y), col_border);

    const float rail_btn_x = 16.0f;
    const float rail_btn_w = rail_w - 32.0f;
    const float btn_h = 40.0f;

    // 分段标题
    ImGui::SetCursorPos(ImVec2(rail_btn_x, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    ImGui::TextUnformatted("开始");
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);

    // 主操作：新建项目
    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.60f, 1.00f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 1.00f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.50f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    if (ImGui::Button("+  新建项目", ImVec2(rail_btn_w, btn_h))) {
        show_new_project_ = true;
    }
    ImGui::PopStyleColor(4);

    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("打开现有项目", ImVec2(rail_btn_w, btn_h))) {
        const std::string folder = pick_folder();
        if (!folder.empty()) open_project(folder);
    }
    ImGui::PopStyleColor(3);

    // 分隔线
    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Separator, col_border);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 选中项目的附加操作
    const bool has_selection = selected_project_index_ >= 0 &&
                               selected_project_index_ < (int)recent_.size();
    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    ImGui::TextUnformatted("操作");
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    ImGui::BeginDisabled(!has_selection);

    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("打开项目", ImVec2(rail_btn_w, btn_h))) {
        open_project(recent_[selected_project_index_]);
    }
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    if (ImGui::Button("打开所在目录", ImVec2(rail_btn_w, btn_h))) {
        open_in_explorer(recent_[selected_project_index_]);
    }
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPosX(rail_btn_x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 0.32f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.72f, 1.0f));
    if (ImGui::Button("从列表移除", ImVec2(rail_btn_w, btn_h))) {
        recent_.erase(recent_.begin() + selected_project_index_);
        save_recent(recent_);
        selected_project_index_ = -1;
        status_ = "已从列表移除";
        status_error_ = false;
    }
    ImGui::PopStyleColor(3);

    ImGui::EndDisabled();
    ImGui::EndChild(); // left_rail

    // ===== 主内容区：搜索 + 最近项目列表 =====
    const float list_x = rail_w;
    const float list_w = width - rail_w;
    ImGui::SetCursorPos(ImVec2(list_x, body_y));
    ImGui::BeginChild("main_panel", ImVec2(list_w, body_h), false);

    // 标题 + 工具栏：搜索 / 排序
    ImGui::Text("最近项目");
    ImGui::SameLine(list_w - 340.0f);

    const char* sort_labels[] = {"最近编辑", "名称", "路径"};
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text);
    ImGui::SetNextItemWidth(130.0f);
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

    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
    ImGui::InputTextWithHint("##filter", "搜索...", filter_text_.data(),
                             filter_text_.size());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::Separator();

    // 过滤 + 排序
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
    if (sort_mode_ == 1) {
        std::sort(indices.begin(), indices.end(), [this](int a, int b) {
            return fs::path(recent_[a]).filename().string() <
                   fs::path(recent_[b]).filename().string();
        });
    } else if (sort_mode_ == 2) {
        std::sort(indices.begin(), indices.end(),
                  [this](int a, int b) { return recent_[a] < recent_[b]; });
    }

    ImGui::BeginChild("project_list", ImVec2(list_w, 0), false);

    if (indices.empty()) {
        const float cx = list_w * 0.5f;
        const float cy = ImGui::GetContentRegionAvail().y * 0.4f;
        ImGui::SetCursorPos(ImVec2(cx - 80.0f, cy));
        ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
        ImGui::TextUnformatted("还没有项目");
        ImGui::SetCursorPos(ImVec2(cx - 130.0f, cy + 26.0f));
        ImGui::TextUnformatted("点击左侧“+ 新建项目”开始");
        ImGui::PopStyleColor();
    } else {
        // 项目卡片
        std::string open_path;
        const float card_h = 84.0f;
        const float inner_w = list_w - 8.0f;
        ImDrawList* cdl = ImGui::GetWindowDrawList();
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
                                  ImVec2(inner_w, card_h))) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    open_path = recent_[idx];
                } else {
                    selected_project_index_ = idx;
                }
            }

            const ImVec2 p1(p0.x + inner_w, p0.y + card_h);
            cdl->AddRectFilled(p0, p1,
                               is_selected ? col_accent_dim : col_card, 6.0f);
            cdl->AddRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1),
                         is_selected ? col_accent : col_card_b, 6.0f);

            // 缩略图（蓝渐变方块图标）
            const ImVec2 thumb(p0.x + 18.0f, p0.y + 12.0f);
            cdl->AddRectFilled(thumb, thumb + ImVec2(60, 60), col_card_b, 10.0f);
            cdl->AddRectFilled(thumb + ImVec2(14, 14), thumb + ImVec2(46, 46),
                               col_accent, 8.0f);
            cdl->AddText(thumb + ImVec2(22, 13), IM_COL32(255, 255, 255, 255), "G");

            // 名称 + 路径
            cdl->AddText(ImVec2(p0.x + 96.0f, p0.y + 14.0f), col_text, name.c_str());
            draw_folder_icon(cdl, ImVec2(p0.x + 96.0f, p0.y + 46.0f), 17.0f,
                             col_text_dim);
            cdl->AddText(ImVec2(p0.x + 118.0f, p0.y + 46.0f), col_text_dim,
                         path.c_str());

            // 右侧：版本 + 最近编辑
            cdl->AddText(ImVec2(p1.x - 190.0f, p0.y + 16.0f), col_text_dim, "v0.1.0");
            cdl->AddText(ImVec2(p1.x - 130.0f, p0.y + 40.0f), col_text_dim,
                         date.c_str());

            ImGui::PopID();

            if (!open_path.empty()) {
                open_project(open_path);
                break;
            }
        }
    }

    ImGui::EndChild(); // project_list
    ImGui::EndChild(); // main_panel

    // ===== 状态栏 =====
    dl->AddRectFilled(win_pos + ImVec2(0, status_y),
                      win_pos + ImVec2(width, height), col_header);
    dl->AddRectFilled(win_pos + ImVec2(0, status_y),
                      win_pos + ImVec2(width, status_y + 1), col_border);

    char count_buf[64];
    std::snprintf(count_buf, sizeof(count_buf), "%zu 个项目", recent_.size());
    ImGui::SetCursorPos(ImVec2(12.0f, status_y + 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, col_text_dim);
    ImGui::TextUnformatted(count_buf);
    ImGui::PopStyleColor();

    if (!status_.empty()) {
        const float status_w = ImGui::CalcTextSize(status_.c_str()).x;
        ImGui::SetCursorPos(ImVec2(width * 0.5f - status_w * 0.5f, status_y + 2.0f));
        ImGui::TextColored(status_error_ ? ImVec4(0.85f, 0.35f, 0.30f, 1.0f)
                                          : ImVec4(0.30f, 0.75f, 0.45f, 1.0f),
                           "%s", status_.c_str());
    }

    ImGui::SetCursorPos(ImVec2(width - 72.0f, status_y + 2.0f));
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

    fs::path out_dir;
    if (!launcher::create_project_dir(name, parent, error, out_dir)) {
        return false;
    }
    open_project(out_dir.string());
    return true;
}

void LauncherApp::open_project(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        status_ = "不是有效目录: " + path;
        status_error_ = true;
        return;
    }
    // 追加默认启动参数与游戏版本
    std::string extra = settings_.editorArgs;
    if (!settings_.lastVersion.empty()) {
        if (!extra.empty()) extra += " ";
        extra += "--version " + settings_.lastVersion;
    }
    std::string error;
    if (!launch_editor(path, error, extra)) {
        status_ = error;
        status_error_ = true;
        return;
    }
    push_recent(recent_, path);
    status_ = "已启动 " + path;
    status_error_ = false;
    // 保持启动器运行，不退出
}

void LauncherApp::render_settings_dialog() {
    ImGui::OpenPopup("启动器设置");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("启动器设置", &show_settings_,
                               ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoScrollbar)) {
        const float label_w = 150.0f;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("默认新建项目目录");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##set_default_dir", set_default_dir_.data(),
                         set_default_dir_.size());

        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("默认启动参数");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputTextWithHint("##set_editor_args", "如 --fullscreen", 
                                 set_editor_args_.data(), set_editor_args_.size());

        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("默认游戏版本");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##set_version", set_version_.data(), set_version_.size());

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float btn_w = 120.0f;
        ImGui::SetCursorPosX(540.0f - 20.0f - btn_w * 2.0f - 12.0f);
        if (ImGui::Button("取消", ImVec2(btn_w, 40.0f))) {
            show_settings_ = false; // 放弃编辑
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.60f, 1.00f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.65f, 1.00f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.50f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        if (ImGui::Button("保存", ImVec2(btn_w, 40.0f))) {
            settings_.defaultDir = std::string(set_default_dir_.data());
            settings_.editorArgs = std::string(set_editor_args_.data());
            settings_.lastVersion = std::string(set_version_.data());
            launcher::save_settings(settings_);
            show_settings_ = false;
            status_ = "设置已保存";
            status_error_ = false;
        }
        ImGui::PopStyleColor(4);

        ImGui::EndPopup();
    }
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
