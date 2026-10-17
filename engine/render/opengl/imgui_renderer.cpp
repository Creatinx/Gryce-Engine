// imgui_renderer.cpp — ImGuiRenderer：引擎内置的 ImGui 封装（GLFW 平台层 +
// 各后端 IImGuiBackend），自持上下文与等宽字体图集，把 NewFrame→Render 产出
// 的 draw data 经回调交给渲染线程执行；宿主自持上下文时亦可复用其面板绘制。
#include "imgui_renderer.h"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <memory>
#include <vector>

#include "render/imgui_backend.h"
#include "render/imgui_font_data.h"
#include "utils/glog/glog_lib.h"

namespace {

// 调试 UI 默认字号（px）。Cousine 在 13~14px 与旧默认（ProggyForever 13px）
// 的 x-height 接近；14px 让等宽字体在小面板里更易读。
constexpr float kDebugUIFontSize = 14.0f;

struct ImDrawListDeleter {
    void operator()(ImDrawList* list) const {
        if (list) {
            IM_DELETE(list);
        }
    }
};
using OwnedDrawList = std::unique_ptr<ImDrawList, ImDrawListDeleter>;

struct ImVectorTexturesDeleter {
    void operator()(ImVector<ImTextureData*>* v) const {
        if (v) IM_DELETE(v);
    }
};

struct OwnedDrawData {
    ImDrawData data;
    std::vector<OwnedDrawList> lists;
    std::unique_ptr<ImVector<ImTextureData*>, ImVectorTexturesDeleter> textures;
};

} // namespace

namespace gryce_engine::render {

ImGuiRenderer::ImGuiRenderer() = default;

ImGuiRenderer::~ImGuiRenderer() {
    shutdown();
}

bool ImGuiRenderer::init(GLFWwindow* window, std::unique_ptr<IImGuiBackend> backend) {
    if (initialized_ || !window || !backend) return false;
    window_ = window;
    backend_ = std::move(backend);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Viewports 需要渲染线程持有 context，当前架构下在 main 线程处理会无 context，先关闭
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    apply_engine_style();
    setup_debug_font(io);

    if (backend_->is_vulkan()) {
        ImGui_ImplGlfw_InitForVulkan(window, true);
    } else if (backend_->is_d3d12()) {
        // D3D12 后端没有对应的 ImGui_ImplGlfw_InitForDX12 —— 用 "Other" 变体，
        // 它只做平台输入（键鼠/光标），渲染由 imgui_impl_dx12 负责。
        ImGui_ImplGlfw_InitForOther(window, true);
    } else {
        ImGui_ImplGlfw_InitForOpenGL(window, true);
    }

    if (!backend_->init()) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        backend_.reset();
        window_ = nullptr;
        return false;
    }

    initialized_ = true;
    GLOG_INFO("ImGuiRenderer initialized (GLFW + backend, docking enabled)");
    return true;
}

void ImGuiRenderer::shutdown() {
    if (!initialized_) return;
    if (backend_) {
        backend_->shutdown();
        backend_.reset();
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
    window_ = nullptr;
}

void ImGuiRenderer::begin_frame() {
    if (!initialized_ || !backend_) return;

    // 在 ImGui::NewFrame() 之前等待上一帧渲染命令（含纹理上传/销毁）完成。
    // ImGui 的字体 atlas 可能在 NewFrame()/Render() 中重建，会读写 ImTextureData 状态；
    // 必须确保渲染线程不再访问这些对象，才能避免主线程与渲染线程竞争。
    if (prev_sync_future_.valid()) {
        prev_sync_future_.wait();
    }

    backend_->new_frame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiRenderer::end_frame(std::function<void(ImDrawData*, std::shared_ptr<std::promise<void>>)> render_callback) {
    if (!initialized_ || !backend_) return;

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();

    auto sync_promise = std::make_shared<std::promise<void>>();
    prev_sync_future_ = sync_promise->get_future();

    if (render_callback) {
        render_callback(draw_data, sync_promise);
    }

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable && draw_data) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
}

std::shared_ptr<ImDrawData> ImGuiRenderer::clone_draw_data(ImDrawData* src) {
    auto owned = std::make_shared<OwnedDrawData>();
    owned->data.Clear();
    owned->data.Valid = src->Valid;
    owned->data.DisplayPos = src->DisplayPos;
    owned->data.DisplaySize = src->DisplaySize;
    owned->data.FramebufferScale = src->FramebufferScale;
    owned->data.OwnerViewport = src->OwnerViewport;

    // 拷贝 Textures 列表（指针），后端渲染时会根据它创建/更新 GPU 纹理。
    // 注意：ImTextureData 对象仍由 ImGui atlas 拥有，渲染线程修改其状态。
    // begin_frame() 会在纹理未就绪时等待上一帧渲染完成，避免竞争。
    if (src->Textures) {
        owned->textures.reset(IM_NEW(ImVector<ImTextureData*>));
        owned->data.Textures = owned->textures.get();
        owned->data.Textures->resize(0);
        for (int i = 0; i < src->Textures->Size; ++i) {
            owned->data.Textures->push_back((*src->Textures)[i]);
        }
    } else {
        owned->data.Textures = nullptr;
    }

    for (int i = 0; i < src->CmdLists.Size; ++i) {
        ImDrawList* cloned = src->CmdLists[i]->CloneOutput();
        owned->data.TotalVtxCount += cloned->VtxBuffer.Size;
        owned->data.TotalIdxCount += cloned->IdxBuffer.Size;
        owned->data.CmdLists.push_back(cloned);
        owned->lists.emplace_back(cloned);
    }
    owned->data.CmdListsCount = owned->data.CmdLists.Size;

    return std::shared_ptr<ImDrawData>(owned, &owned->data);
}

void ImGuiRenderer::render_draw_data(ImDrawData* draw_data) {
    if (backend_) {
        backend_->render_draw_data(draw_data);
    }
}

void ImGuiRenderer::setup_debug_font(ImGuiIO& io) {
    // -----------------------------------------------------------------------
    // 等宽字体 + 抗锯齿
    //
    // 默认字体是 ProggyForever（比例宽度）。调试 UI 里有大量对齐的数字列
    // （FPS / 帧时间直方图 / 参数表），比例字体会逐帧抖动，换成等宽字体后
    // 列对齐稳定、数值变化不再引起整行重排。
    //
    // Cousine 以编译期数组内嵌（imgui_font_data.h），不依赖运行时字体文件。
    //
    // 抗锯齿分三层，全部显式打开：
    //   1. 栅格化过采样：OversampleH=2（14px 小字号的自动值也是 2，这里显式
    //      固定，避免将来改字号时静默变成 1）；OversampleV=1（Y 轴无亚像素
    //      定位，过采样无收益）。
    //   2. 图集双线性过滤：三个后端默认即双线性 —— GL 建纹理时 GL_LINEAR
    //      （imgui_impl_opengl3.cpp），Vulkan 渲染开始绑定 SamplerLinearDS
    //      （imgui_impl_vulkan.cpp），D3D12 根签名静态采样器
    //      MIN_MAG_MIP_LINEAR（imgui_impl_dx12.cpp "Bilinear sampling is
    //      required"）。因此**不能**给 atlas 设 ImFontAtlasFlags_NoBakedLines
    //      —— 那会让线条退化为多边形且禁用 AntiAliasedLinesUseTex。
    //   3. 绘制标志：AntiAliasedLines / LinesUseTex / Fill 显式置 true
    //      （虽与默认一致，写出来防止将来 StyleColorsDark 变体改动默认）。
    // -----------------------------------------------------------------------
    ImFontConfig font_cfg;
    // 静态内嵌数据的生命周期贯穿整个进程：绝不能让 atlas 在析构时 free 它。
    font_cfg.FontDataOwnedByAtlas = false;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(static_cast<const void*>(kCousineRegularTtf)),
        static_cast<int>(kCousineRegularTtfSize), kDebugUIFontSize, &font_cfg);
    if (!font) {
        // 内嵌数据损坏等极端情况：退回默认字体，UI 仍可用（退化为比例宽度）。
        GLOG_WARN("ImGuiRenderer: failed to load embedded mono font, "
                  "falling back to default font");
        io.Fonts->AddFontDefault();
        return;
    }
    io.FontDefault = font;

    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
}

void ImGuiRenderer::apply_engine_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // 圆角 + 间距（参考 macOS：轻微圆角，更专业的边框）
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarSize = 16.0f;
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    // 引擎暗色主题
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]               = ImVec4(0.10f, 0.11f, 0.13f, 0.95f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.10f, 0.11f, 0.13f, 0.95f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.13f, 0.15f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.25f, 0.26f, 0.28f, 0.50f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.18f, 0.19f, 0.22f, 0.80f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.25f, 0.26f, 0.30f, 0.80f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.32f, 0.36f, 0.80f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.45f, 0.80f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.25f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.15f, 0.40f, 0.75f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.45f, 0.80f, 0.55f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.25f, 0.55f, 0.95f, 0.65f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.15f, 0.40f, 0.75f, 0.75f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.20f, 0.45f, 0.80f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.25f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.25f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.20f, 0.45f, 0.80f, 0.55f);
    colors[ImGuiCol_DockingPreview]         = ImVec4(0.25f, 0.55f, 0.95f, 0.50f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
}

} // namespace gryce_engine::render
