#pragma once

#include <functional>

#include "../editor_panel.h"

#include "math/math.h"

namespace gryce_engine {
namespace math { class Camera; }
namespace scene { class Entity; }
namespace render { class RenderPipeline; class IImGuiBackend; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ViewportPanel — 场景视口面板（M1-E2）
// ImGui::Image 嵌入 tonemap 输出纹理；叠加 ImGuizmo 变换 gizmo
// （W/E/R 切换平移/旋转/缩放）；左击上报拾取 UV 给 EditorApp 做射线拾取。
// 拾取与 gizmo 只在非 Play 状态生效（Play Mode 预留，见 set_editing_enabled）。
// ---------------------------------------------------------------------------
class ViewportPanel : public EditorPanel {
public:
    ViewportPanel();

    void set_pipeline(render::RenderPipeline* pipeline) { pipeline_ = pipeline; }
    void set_imgui_backend(render::IImGuiBackend* backend) { backend_ = backend; }
    void set_camera(math::Camera* camera) { camera_ = camera; }
    // 选中实体提供者（Hierarchy 的 UUID 弱引用解析结果）
    void set_selection_provider(std::function<scene::Entity*()> provider) {
        selection_provider_ = std::move(provider);
    }
    // 编辑使能：Play Mode 下置 false 关闭拾取与 gizmo（预留接口，当前恒 true）
    void set_editing_enabled(bool enabled) { editing_enabled_ = enabled; }

    bool hovered() const { return hovered_; }

    // gizmo 正在拖拽或悬停：EditorApp 据此屏蔽相机操作与点选拾取
    bool gizmo_active() const { return gizmo_active_; }

    // 上一帧面板内容区尺寸（像素）；EditorApp 据此调整渲染目标与相机宽高比
    float content_width() const { return size_.x; }
    float content_height() const { return size_.y; }

    // 取走本帧的拾取点击（UV ∈ [0,1]，原点左上）。无点击返回 false。
    bool take_pick_click(ImVec2& out_uv);

    // 资源拖放回调：用户从 Project 面板拖文件到视口时触发（参数为 res:/ 路径）
    void set_drop_handler(std::function<void(const std::string&)> handler) {
        drop_handler_ = std::move(handler);
    }

protected:
    void on_imgui() override;

private:
    void draw_gizmo();
    void handle_gizmo_shortcuts();

    render::RenderPipeline* pipeline_ = nullptr;
    render::IImGuiBackend* backend_ = nullptr;
    math::Camera* camera_ = nullptr;
    std::function<scene::Entity*()> selection_provider_;
    std::function<void(const std::string&)> drop_handler_;

    bool hovered_ = false;
    bool gizmo_active_ = false;
    bool editing_enabled_ = true;
    ImVec2 size_ = ImVec2(0.0f, 0.0f);
    ImVec2 image_min_ = ImVec2(0.0f, 0.0f);
    bool image_drawn_ = false;

    bool pick_click_pending_ = false;
    ImVec2 pick_click_uv_ = ImVec2(0.0f, 0.0f);

    int gizmo_operation_; // ImGuizmo::OPERATION，构造时初始化（避免头文件依赖 ImGuizmo）
};

} // namespace gryce_engine::editor
