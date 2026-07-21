#include "imgui_renderer.h"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <memory>
#include <vector>

#include "render/imgui_backend.h"
#include "utils/glog/glog_lib.h"

namespace {

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

    if (backend_->is_vulkan()) {
        ImGui_ImplGlfw_InitForVulkan(window, true);
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
