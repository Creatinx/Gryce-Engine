// ===========================================================================
// editor/ext/view/viewport.h — 视口状态
//
// 引擎把视口能力拆得很细（GRender_SetViewportOutput / GetViewportTexture /
// GViewport_SetEditorCamera / GEdit_SetRenderTargetSize），但没有"当前视口
// 是什么状态"这个聚合概念。编辑器需要一个地方回答"多大、开没开离屏、
// 现在什么显示模式"，这里就是。
//
// 显示模式由 ext 自己记：引擎只有 Set 没有 Get，而面板要显示当前模式。
// ===========================================================================

#ifndef GRYCE_EDITOR_VIEWPORT_H
#define GRYCE_EDITOR_VIEWPORT_H

#include "api/types.h"

#include <string>

namespace gryce::editor::view {

// 视口相机参数。引擎的 GViewport_SetEditorCamera 只写不读，而拾取需要
// 从**视口相机**发射射线（场景里的 Camera 实体是被编辑的内容，它的朝向
// 与视口无关）。所以这里存一份副本，供 screen_ray 使用。
struct EditorCamera {
    float pos[3] = {0.0f, 2.0f, 6.0f};
    float yaw = 0.0f;      // 度，绕 Y 轴
    float pitch = -12.0f;  // 度，绕局部 X 轴
    float fov = 55.0f;     // 垂直视场角
    bool valid = false;
};

void reset();

int enable_output(int enabled);
int set_size(int w, int h);
void set_editor_camera(float x, float y, float z, float yaw_deg, float pitch_deg,
                       float fov_deg);

// 视口相机状态（未设置过时 valid = 0）。
const EditorCamera& editor_camera();

// 屏幕坐标 → 世界射线。sx/sy 以视口左上角为原点。
// 返回 0 表示相机还没设置过（此时 pick 应当直接放弃）。
int screen_ray(float sx, float sy, GVec3* out_origin, GVec3* out_dir);

int set_display_mode(const char* mode);
int display_mode_supported(const char* mode);
const char* display_mode();

// 视口纹理与 GameView 纹理（编辑器贴到自己面板里用）。
GTextureHandle viewport_texture();
GTextureHandle game_view_texture();

// 每帧：视口尺寸被 renderer 采纳 / 被窗口尺寸夹住时同步回来。
void tick();

std::string info_json();

} // namespace gryce::editor::view

#endif // GRYCE_EDITOR_VIEWPORT_H
