#pragma once

#include <string>
#include <vector>

#include "components/skinned_mesh_renderer.h"

namespace gryce_engine {
namespace scene { class Entity; }
namespace editor {

// ---------------------------------------------------------------------------
// AnimationEditorWindow — 动画编辑器（M2 基础版）
//
// 当前实现聚焦于动画播放控制：
// - 列出 SkinnedMeshRenderer 关联模型中的所有动画剪辑
// - 切换当前播放的 clip
// - 播放/暂停、循环、速度、时间滑块
//
// 关键帧曲线编辑依赖更完整的动画数据持久化，排入后续迭代。
// ---------------------------------------------------------------------------
class AnimationEditorWindow {
public:
    AnimationEditorWindow() = default;

    void open(scene::Entity* entity);
    void draw();
    bool is_open() const { return open_; }

private:
    bool open_ = false;
    scene::Entity* entity_ = nullptr;

    void draw_clip_list(components::SkinnedMeshRenderer* smr);
    void draw_playback_controls(components::SkinnedMeshRenderer* smr);
};

} // namespace editor
} // namespace gryce_engine
