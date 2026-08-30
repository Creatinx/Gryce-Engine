#pragma once

#include <memory>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace gryce_engine::ecs {
class World;
}

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// SceneManager — 多场景打开、切换、保存/加载
// ---------------------------------------------------------------------------
class SceneManager {
public:
    struct OpenScene {
        std::unique_ptr<scene::Scene> scene;
        std::string path;
        std::string title;
        bool dirty = false;
    };

    bool new_scene(ecs::World* world, const std::string& title = "Untitled");
    bool open_scene(ecs::World* world, const std::string& path);
    bool save_active(const std::string& path = {});
    bool save_scene(int index, const std::string& path = {});
    bool close_scene(int index, ecs::World* world);
    bool activate_scene(int index, ecs::World* world);
    void replace_active(ecs::World* world,
                        std::unique_ptr<scene::Scene> replacement);
    void detach_active_from_world(ecs::World* world);

    scene::Scene* active_scene() const;
    int active_index() const { return active_index_; }
    bool has_active() const;
    const std::vector<OpenScene>& scenes() const { return scenes_; }
    std::string active_path() const;

    void mark_active_dirty() {
        if (active_index_ >= 0 && active_index_ < static_cast<int>(scenes_.size())) {
            scenes_[active_index_].dirty = true;
        }
    }

private:
    void transfer_active_into_slot(ecs::World* world);
    void attach_index(ecs::World* world, int index);
    std::string title_from_path(const std::string& path) const;

    std::vector<OpenScene> scenes_;
    int active_index_ = -1;
};

} // namespace gryce_engine::editor
