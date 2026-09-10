#pragma once

#include <memory>
#include <string>

#include "scene/scene.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// PlayModeManager — 编辑器 Edit <-> Play 状态与场景快照
// ---------------------------------------------------------------------------
class PlayModeManager {
public:
    bool is_playing() const { return playing_; }
    bool is_paused() const { return paused_; }

    void begin_play(scene::Scene* scene);
    std::unique_ptr<scene::Scene> end_play();

    void toggle_pause();
    void request_step();
    bool consume_step_request();

private:
    bool playing_ = false;
    bool paused_ = false;
    bool step_requested_ = false;
    std::string snapshot_json_;
};

} // namespace gryce_engine::editor
