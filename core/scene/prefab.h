#pragma once

#include <memory>
#include <string>
#include <vector>

#include "scene/entity.h"

namespace gryce_engine::scene {

class Scene;

// ---------------------------------------------------------------------------
// Prefab — 预制体模板，支持从 .gesc 文件加载并实例化到任意 Scene。
// 实例化时生成全新的 UUID 与 EntityID，组件数据通过序列化深拷贝。
// ---------------------------------------------------------------------------
class Prefab {
public:
    Prefab() = default;
    ~Prefab() = default;

    Prefab(const Prefab&) = delete;
    Prefab& operator=(const Prefab&) = delete;
    Prefab(Prefab&&) = default;
    Prefab& operator=(Prefab&&) = default;

    // 从 .gesc 文件路径加载预制体（不关联任何 Scene）
    static std::unique_ptr<Prefab> load(const std::string& path);

    // 实例化到目标场景，返回第一个根实体（若无根则返回 nullptr）
    Entity* instantiate(Scene* scene) const;

    const std::vector<std::unique_ptr<Entity>>& roots() const { return roots_; }

private:
    std::vector<std::unique_ptr<Entity>> roots_;
};

} // namespace gryce_engine::scene
