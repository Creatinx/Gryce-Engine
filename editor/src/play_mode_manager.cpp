#include "play_mode_manager.h"

#include <nlohmann/json.hpp>

#include "scene/scene_serializer.h"

namespace gryce_engine::editor {

void PlayModeManager::begin_play(scene::Scene* scene) {
    if (playing_ || !scene) return;
    snapshot_json_ = scene::SceneSerializer::serialize(*scene).dump(2);
    playing_ = true;
    paused_ = false;
    step_requested_ = false;
}

std::unique_ptr<scene::Scene> PlayModeManager::end_play() {
    if (!playing_) return nullptr;
    playing_ = false;
    paused_ = false;
    step_requested_ = false;

    if (snapshot_json_.empty()) return nullptr;
    try {
        auto json = nlohmann::json::parse(snapshot_json_);
        auto restored = scene::SceneSerializer::deserialize(json);
        snapshot_json_.clear();
        return restored;
    } catch (...) {
        snapshot_json_.clear();
        return nullptr;
    }
}

void PlayModeManager::toggle_pause() {
    if (!playing_) return;
    paused_ = !paused_;
    if (!paused_) {
        step_requested_ = false;
    }
}

void PlayModeManager::request_step() {
    if (!playing_ || !paused_) return;
    step_requested_ = true;
}

bool PlayModeManager::consume_step_request() {
    const bool step = step_requested_;
    step_requested_ = false;
    return step;
}

} // namespace gryce_engine::editor
