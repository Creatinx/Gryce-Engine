#include "ecs/world.h"

#include <algorithm>

#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

World::World() = default;
World::~World() {
    shutdown();
}

void World::attach_scene(std::unique_ptr<scene::Scene> scene) {
    if (initialized_ && scene_) {
        GLOG_WARN("World::attach_scene: replacing active scene, call shutdown first for clean teardown");
        shutdown();
    }
    scene_ = std::move(scene);
    if (scene_ && initialized_) {
        scene_->init();
        for (const auto& sys : systems_) {
            if (sys->enabled) {
                sys->on_init(*scene_);
            }
        }
    }
}

std::unique_ptr<scene::Scene> World::detach_scene() {
    if (initialized_) {
        shutdown();
    }
    return std::move(scene_);
}

void World::register_system(std::unique_ptr<ISystem> system) {
    if (!system) return;
    if (system_map_.find(system->name()) != system_map_.end()) {
        GLOG_WARN("World::register_system: system '{}' already registered, ignoring", system->name());
        return;
    }
    system_map_[system->name()] = system.get();
    systems_.push_back(std::move(system));
}

ISystem* World::get_system(const std::string& name) const {
    auto it = system_map_.find(name);
    return it != system_map_.end() ? it->second : nullptr;
}

void World::init() {
    if (initialized_) return;
    if (!scene_) {
        GLOG_WARN("World::init: no scene attached");
    }
    if (scene_) {
        scene_->init();
    }
    for (const auto& sys : systems_) {
        if (sys->enabled && scene_) {
            sys->on_init(*scene_);
        }
    }
    initialized_ = true;
    GLOG_INFO("World initialized with {} systems", systems_.size());
}

void World::shutdown() {
    if (!initialized_) return;
    for (const auto& sys : systems_) {
        if (scene_) {
            sys->on_shutdown(*scene_);
        }
    }
    if (scene_) {
        scene_->destroy();
    }
    initialized_ = false;
    GLOG_INFO("World shutdown");
}

// 按 phase 分组后，同 phase 内按 priority 降序排序
static void sort_systems_by_priority(std::vector<std::unique_ptr<ISystem>>& systems) {
    std::stable_sort(systems.begin(), systems.end(),
        [](const std::unique_ptr<ISystem>& a, const std::unique_ptr<ISystem>& b) {
            if (a->phase() != b->phase()) {
                return static_cast<int>(a->phase()) < static_cast<int>(b->phase());
            }
            return a->priority() > b->priority(); // 数值越大越先执行
        });
}

void World::update(float dt) {
    if (!initialized_ || !scene_ || !updates_enabled_) return;
    // 先驱动组件级 update，再按阶段跑 System update
    scene_->update(dt);
    for (auto phase : {ISystem::Phase::PreUpdate, ISystem::Phase::Update, ISystem::Phase::PostUpdate}) {
        for (const auto& sys : systems_) {
            if (sys->enabled && sys->phase() == phase) {
                sys->on_update(*scene_, dt);
            }
        }
    }
}

void World::render(render::RenderContext& ctx) {
    if (!initialized_ || !scene_) return;
    for (auto phase : {ISystem::Phase::PreRender, ISystem::Phase::Render, ISystem::Phase::PostRender}) {
        for (const auto& sys : systems_) {
            if (sys->enabled && sys->phase() == phase) {
                sys->on_render(*scene_, ctx);
            }
        }
    }
    // 组件级 render 作为补充（自定义组件可自行绘制）
    scene_->render(ctx);
}

} // namespace gryce_engine::ecs
