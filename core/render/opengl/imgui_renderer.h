#pragma once

#include <functional>
#include <memory>
#include <future>

struct ImDrawData;
struct GLFWwindow;

namespace gryce_engine::render {

class IImGuiBackend;

// ---------------------------------------------------------------------------
// ImGuiRenderer — Dear ImGui GLFW 平台封装 + 可切换渲染后端
// 注意：end_frame 生成 draw data 后通过回调把实际渲染提交到渲染线程。
// ---------------------------------------------------------------------------
class ImGuiRenderer {
public:
    ImGuiRenderer();
    ~ImGuiRenderer();

    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    // 初始化：window 为 GLFW 窗口，backend 为 API 相关渲染后端（OpenGL/Vulkan）
    bool init(GLFWwindow* window, std::unique_ptr<IImGuiBackend> backend);
    void shutdown();

    // 等待上一帧 ImGui 渲染完成后开始新帧，避免 NewFrame()/Render() 重建 atlas 时
    // 与渲染线程竞争 ImTextureData 的状态。
    void begin_frame();
    // render_callback 会收到一个 std::promise<void>，调用方需要在 ImGui 渲染命令
    // 执行完成后调用 set_value()，以便下一帧 begin_frame() 等待纹理处理完成。
    void end_frame(std::function<void(ImDrawData*, std::shared_ptr<std::promise<void>>)> render_callback);

    // 深拷贝一份 ImDrawData，供异步渲染线程使用。
    // 返回的 shared_ptr 保证内部 CmdLists 生命周期延续到引用释放。
    std::shared_ptr<ImDrawData> clone_draw_data(ImDrawData* src);

    // 直接让后端渲染 draw data（用于推入渲染线程）
    void render_draw_data(ImDrawData* draw_data);

    // 访问渲染后端（编辑器 Viewport 面板查询纹理 ID 用）
    IImGuiBackend* backend() const { return backend_.get(); }

    bool initialized() const { return initialized_; }

private:
    void apply_engine_style();

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<IImGuiBackend> backend_;
    bool initialized_ = false;

    // 上一帧 ImGui 渲染完成的信号，用于在下一帧 ImGui::Render() 前同步纹理状态
    std::future<void> prev_sync_future_;
};

} // namespace gryce_engine::render
