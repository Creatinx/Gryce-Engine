#include "scene_manager.h"

#include <filesystem>

#include "ecs/world.h"
#include "scene/scene_serializer.h"

namespace gryce_engine::editor {

namespace fs = std::filesystem;

void SceneManager::transfer_active_into_slot(ecs::World* world) {
    if (active_index_ < 0 || active_index_ >= static_cast<int>(scenes_.size())) return;
    if (world && world->scene() == scenes_[active_index_].scene.get()) return;

    if (world) {
        auto detached = world->detach_scene();
        if (detached) {
            scenes_[active_index_].scene = std::move(detached);
        }
    }
}

void SceneManager::attach_index(ecs::World* world, int index) {
    if (!world || index < 0 || index >= static_cast<int>(scenes_.size())) return;
    if (!scenes_[index].scene) return;
    world->attach_scene(std::move(scenes_[index].scene));
}

bool SceneManager::new_scene(ecs::World* world, const std::string& title) {
    if (!world) return false;
    transfer_active_into_slot(world);

    OpenScene open;
    open.scene = std::make_unique<scene::Scene>(title);
    open.title = title;
    open.dirty = true;
    scenes_.push_back(std::move(open));
    active_index_ = static_cast<int>(scenes_.size()) - 1;
    attach_index(world, active_index_);
    return true;
}

bool SceneManager::open_scene(ecs::World* world, const std::string& path) {
    if (!world) return false;

    // 已打开的场景直接切换到对应标签。
    for (int i = 0; i < static_cast<int>(scenes_.size()); ++i) {
        if (scenes_[i].path == path) {
            return activate_scene(i, world);
        }
    }

    auto loaded = scene::SceneSerializer::load_from_file(path);
    if (!loaded) return false;
    loaded->mark_saved();

    transfer_active_into_slot(world);

    OpenScene open;
    open.scene = std::move(loaded);
    open.path = path;
    open.title = title_from_path(path);
    open.dirty = false;
    scenes_.push_back(std::move(open));
    active_index_ = static_cast<int>(scenes_.size()) - 1;
    attach_index(world, active_index_);
    return true;
}

bool SceneManager::save_active(const std::string& path) {
    return save_scene(active_index_, path);
}

bool SceneManager::save_scene(int index, const std::string& path) {
    if (index < 0 || index >= static_cast<int>(scenes_.size())) return false;
    OpenScene& open = scenes_[index];
    if (!open.scene) return false;

    std::string target = path;
    if (target.empty()) {
        target = open.path;
    }
    if (target.empty()) return false;

    if (!scene::SceneSerializer::save_to_file(*open.scene, target)) return false;
    open.path = target;
    open.title = title_from_path(target);
    open.dirty = false;
    open.scene->mark_saved();
    return true;
}

bool SceneManager::close_scene(int index, ecs::World* world) {
    if (index < 0 || index >= static_cast<int>(scenes_.size())) return false;

    if (index == active_index_) {
        if (world) {
            auto detached = world->detach_scene();
            if (detached) {
                scenes_[active_index_].scene = std::move(detached);
            }
        }
        scenes_.erase(scenes_.begin() + index);

        if (scenes_.empty()) {
            active_index_ = -1;
            if (world) {
                auto empty = std::make_unique<scene::Scene>("Untitled");
                scenes_.push_back({std::move(empty), {}, "Untitled", true});
                active_index_ = 0;
                attach_index(world, active_index_);
            }
        } else {
            const int next = std::min(index, static_cast<int>(scenes_.size()) - 1);
            attach_index(world, next);
            active_index_ = next;
        }
    } else {
        if (index < active_index_) {
            --active_index_;
        }
        scenes_.erase(scenes_.begin() + index);
    }
    return true;
}

bool SceneManager::activate_scene(int index, ecs::World* world) {
    if (index < 0 || index >= static_cast<int>(scenes_.size())) return false;
    if (index == active_index_ && world && world->scene() == scenes_[index].scene.get()) {
        return true;
    }

    transfer_active_into_slot(world);
    active_index_ = index;
    attach_index(world, index);
    return true;
}

void SceneManager::replace_active(ecs::World* world,
                                  std::unique_ptr<scene::Scene> replacement) {
    if (active_index_ < 0 || active_index_ >= static_cast<int>(scenes_.size())) {
        if (replacement && world) {
            OpenScene open;
            open.scene = std::move(replacement);
            open.title = "Untitled";
            open.dirty = true;
            scenes_.push_back(std::move(open));
            active_index_ = static_cast<int>(scenes_.size()) - 1;
            attach_index(world, active_index_);
        }
        return;
    }

    if (world) {
        auto old = world->detach_scene();
        (void)old;
    }
    scenes_[active_index_].scene = std::move(replacement);
    if (scenes_[active_index_].scene) {
        scenes_[active_index_].scene->mark_saved();
        attach_index(world, active_index_);
    }
}

void SceneManager::detach_active_from_world(ecs::World* world) {
    transfer_active_into_slot(world);
    active_index_ = -1;
}

scene::Scene* SceneManager::active_scene() const {
    if (active_index_ < 0 || active_index_ >= static_cast<int>(scenes_.size())) {
        return nullptr;
    }
    return scenes_[active_index_].scene.get();
}

bool SceneManager::has_active() const {
    return active_index_ >= 0 && active_index_ < static_cast<int>(scenes_.size());
}

std::string SceneManager::active_path() const {
    if (active_index_ < 0 || active_index_ >= static_cast<int>(scenes_.size())) {
        return {};
    }
    return scenes_[active_index_].path;
}

std::string SceneManager::title_from_path(const std::string& path) const {
    return fs::path(path).stem().string();
}

} // namespace gryce_engine::editor
