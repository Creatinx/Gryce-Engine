#pragma once

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "ecs/system.h"
#include "ecs/types.h"

namespace gryce_engine {
namespace scene { class Scene; }
namespace render { class RenderContext; }
} // namespace gryce_engine

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// World — ECS 世界
// 持有 Scene 和一组 Systems，按阶段驱动所有 System。
// ---------------------------------------------------------------------------
class World {
public:
    World();
    ~World();

    // Scene 管理
    void attach_scene(std::unique_ptr<scene::Scene> scene);
    scene::Scene* scene() const { return scene_.get(); }
    std::unique_ptr<scene::Scene> detach_scene();

    // System 注册
    void register_system(std::unique_ptr<ISystem> system);

    template<typename T, typename... Args>
    T* add_system(Args&&... args) {
        static_assert(std::is_base_of_v<ISystem, T>, "T must derive from ISystem");
        auto sys = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = sys.get();
        register_system(std::move(sys));
        return ptr;
    }

    ISystem* get_system(const std::string& name) const;

    template<typename T>
    T* get_system() const {
        static_assert(std::is_base_of_v<ISystem, T>, "T must derive from ISystem");
        for (const auto& sys : systems_) {
            if (auto* ptr = dynamic_cast<T*>(sys.get())) {
                return ptr;
            }
        }
        return nullptr;
    }

    // 生命周期
    void init();
    void shutdown();

    // 每帧驱动
    void update(float dt);
    void render(render::RenderContext& ctx);

    // 暂停/恢复 System 更新（热重载等场景）
    void set_updates_enabled(bool enabled) { updates_enabled_ = enabled; }
    bool updates_enabled() const { return updates_enabled_; }

private:
    std::unique_ptr<scene::Scene> scene_;
    std::vector<std::unique_ptr<ISystem>> systems_;
    std::unordered_map<std::string, ISystem*> system_map_;
    bool initialized_ = false;
    bool updates_enabled_ = true;
};

} // namespace gryce_engine::ecs
