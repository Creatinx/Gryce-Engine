#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "scene/scene.h"

namespace gryce_engine::scene {

// ---------------------------------------------------------------------------
// SceneSerializer — 场景 JSON 序列化器
// 保存/加载 .gesc 文件，支持 res:/ 路径。
// ---------------------------------------------------------------------------
class SceneSerializer {
public:
    static nlohmann::json serialize(const Scene& scene);
    static std::unique_ptr<Scene> deserialize(const nlohmann::json& json);

    static bool save_to_file(const Scene& scene, const std::string& path);
    static std::unique_ptr<Scene> load_from_file(const std::string& path);

private:
    static nlohmann::json serialize_entity(const Entity& entity);
    static std::unique_ptr<Entity> deserialize_entity(const nlohmann::json& json,
                                                       std::unordered_map<UUID, Entity*>& entity_map);
};

} // namespace gryce_engine::scene
