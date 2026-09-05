#pragma once

// GryceEngineUtils::ecs::world.h — ECS World（解耦版）
//
// 与内部 World 不同，这里的 World 不再自己创建/持有渲染管线：
// 外部注入 Renderer*（不管理其生命周期），update() 内部通过
// renderer->draw() 收集可见物体并直接提交绘制。

#include <memory>

#include "GryceEngineUtils/renderer.h"
#include "GryceEngineUtils/ecs/entity.h"
#include "GryceEngineUtils/ecs/scene.h"

namespace gryce_engine::ecs { class World; }

namespace GryceEngineUtils::ecs {

class World {
public:
    // 注入外部 Renderer（裸框架模式不用 ECS 时，World 完全不参与）
    static World* create(Renderer* renderer);
    void destroy();
    void update(float dt);

    Entity* create_entity(const char* name);
    void    destroy_entity(Entity* entity);
    Scene*  scene();

    Renderer* renderer() const { return renderer_; }

private:
    explicit World(Renderer* renderer);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Renderer* renderer_ = nullptr;
    std::unique_ptr<gryce_engine::ecs::World> inner_;
};

} // namespace GryceEngineUtils::ecs
