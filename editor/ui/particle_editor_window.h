#pragma once

#include "components/2d/particle_emitter.h"

namespace gryce_engine {
namespace scene { class Entity; }
namespace editor {

// ---------------------------------------------------------------------------
// ParticleEditorWindow — 2D 粒子编辑器基础窗口
//
// 为 ParticleEmitter2D 提供集中式参数编辑、颜色/尺寸曲线预览、
// 即时爆发和清空等操作，补充 Inspector 中基于反射的字段编辑。
// ---------------------------------------------------------------------------
class ParticleEditorWindow {
public:
    ParticleEditorWindow() = default;

    void open(scene::Entity* entity, components::d2::ParticleEmitter2D* emitter);
    void draw();
    bool is_open() const { return open_; }

private:
    bool open_ = false;
    scene::Entity* entity_ = nullptr;
    components::d2::ParticleEmitter2D* emitter_ = nullptr;

    void draw_emission();
    void draw_lifetime();
    void draw_velocity();
    void draw_appearance();
    void draw_preview();
    void draw_actions();
};

} // namespace editor
} // namespace gryce_engine
