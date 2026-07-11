#include "scene_serializer.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "components/component_factory.h"
#include "components/transform.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

namespace {

constexpr int k_gesc_version = 1;

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        GLOG_ERROR("SceneSerializer: failed to open file '{}'", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, const std::string& content) {
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream file(path);
    if (!file) {
        GLOG_ERROR("SceneSerializer: failed to write file '{}'", path);
        return false;
    }
    file << content;
    return file.good();
}

} // namespace

nlohmann::json SceneSerializer::serialize(const Scene& scene) {
    nlohmann::json out;
    out["version"] = k_gesc_version;
    out["name"] = scene.name();

    nlohmann::json entities = nlohmann::json::array();
    // const_cast 是安全的：foreach 只读访问
    const_cast<Scene&>(scene).foreach([&](Entity* e) {
        entities.push_back(serialize_entity(*e));
    });
    out["entities"] = entities;

    return out;
}

nlohmann::json SceneSerializer::serialize_entity(const Entity& entity) {
    nlohmann::json out;
    out["name"] = entity.name();
    out["uuid"] = entity.uuid().str();
    out["parent"] = entity.parent() ? nlohmann::json(entity.parent()->uuid().str()) : nlohmann::json(nullptr);

    // Transform 单独序列化（每个 Entity 都有）
    if (auto* t = entity.transform()) {
        nlohmann::json transform_json;
        t->serialize(transform_json);
        out["transform"] = transform_json;
    }

    // 其他组件
    nlohmann::json components = nlohmann::json::array();
    for (const auto& comp : entity.components()) {
        if (comp->type() == std::string("Transform")) continue; // Transform 已单独处理
        nlohmann::json comp_json;
        comp_json["type"] = comp->type();
        comp_json["enabled"] = comp->enabled;
        comp->serialize(comp_json);
        components.push_back(comp_json);
    }
    out["components"] = components;

    return out;
}

std::unique_ptr<Scene> SceneSerializer::deserialize(const nlohmann::json& json) {
    auto scene = std::make_unique<Scene>(json.value("name", "Scene"));

    std::vector<std::unique_ptr<Entity>> all_entities;
    std::unordered_map<UUID, Entity*> entity_map;
    std::unordered_map<UUID, UUID> parent_map; // child -> parent

    const auto& entities_json = json.value("entities", nlohmann::json::array());
    for (const auto& e_json : entities_json) {
        UUID id(e_json.value("uuid", UUID::generate().str()));
        auto entity = std::make_unique<Entity>(e_json.value("name", "Entity"));
        entity->set_uuid(id);

        // 反序列化 Transform
        if (auto* t = entity->transform()) {
            t->deserialize(e_json.value("transform", nlohmann::json::object()));
        }

        // 反序列化其他组件
        const auto& comps_json = e_json.value("components", nlohmann::json::array());
        for (const auto& c_json : comps_json) {
            std::string type = c_json.value("type", "");
            auto comp = components::ComponentFactory::instance().create(type);
            if (comp) {
                comp->enabled = c_json.value("enabled", true);
                comp->deserialize(c_json);
                entity->add_component(std::move(comp));
            }
        }

        if (e_json.contains("parent") && !e_json["parent"].is_null()) {
            parent_map[id] = UUID(e_json["parent"].get<std::string>());
        }

        Entity* raw = entity.get();
        entity_map[id] = raw;
        all_entities.push_back(std::move(entity));
    }

    // 重建父子关系
    for (const auto& [child_id, parent_id] : parent_map) {
        auto child_it = entity_map.find(child_id);
        auto parent_it = entity_map.find(parent_id);
        if (child_it == entity_map.end() || parent_it == entity_map.end()) continue;

        Entity* child_ptr = child_it->second;
        Entity* parent_ptr = parent_it->second;

        // 从 all_entities 中取出 child 的 unique_ptr
        std::unique_ptr<Entity> child_owner;
        for (auto it = all_entities.begin(); it != all_entities.end(); ++it) {
            if (it->get() == child_ptr) {
                child_owner = std::move(*it);
                all_entities.erase(it);
                break;
            }
        }
        if (child_owner) {
            parent_ptr->add_child(std::move(child_owner));
        }
    }

    // 剩下的都是根实体
    for (auto& e : all_entities) {
        scene->add_root_entity(std::move(e));
    }

    return scene;
}

bool SceneSerializer::save_to_file(const Scene& scene, const std::string& path) {
    std::string resolved = resources::ResourcePath::resolve(path);
    std::string content = serialize(scene).dump(2);
    return write_file(resolved, content);
}

std::unique_ptr<Scene> SceneSerializer::load_from_file(const std::string& path) {
    std::string resolved = resources::ResourcePath::resolve(path);
    std::string content = read_file(resolved);
    if (content.empty()) {
        return nullptr;
    }

    try {
        nlohmann::json json = nlohmann::json::parse(content);
        return deserialize(json);
    } catch (const std::exception& e) {
        GLOG_ERROR("SceneSerializer: failed to parse '{}': {}", resolved, e.what());
        return nullptr;
    }
}

} // namespace gryce_engine::scene
