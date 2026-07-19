#include "scene_serializer.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "components/component_factory.h"
#include "components/prefab_instance.h"
#include "components/transform.h"
#include "scene/prefab.h"
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
        // Prefab 实例展开的模板成员不单独序列化（由实例根的紧凑节点表达）；
        // 实例根下运行时添加的子实体嵌套在实例节点的 "children" 里，同样跳过。
        if (!e->prefab_template_uuid().empty()) return;
        for (Entity* p = e->parent(); p != nullptr; p = p->parent()) {
            if (Prefab::get_instance(p) != nullptr) return;
        }
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
    out["enabled"] = entity.enabled;

    // Prefab 实例根：写成紧凑引用形式（prefab 路径 + 覆盖参数 + 成员映射），
    // 模板成员子树不展开；运行时添加的子实体单独序列化在 "children" 里。
    if (auto* inst = Prefab::get_instance(const_cast<Entity*>(&entity))) {
        // members 以当前实例 UUID 为准重建（实例化/加载后可能重映射过）
        Prefab::refresh_members(const_cast<Entity*>(&entity));
        out["prefab"] = inst->prefab_path;
        out["overrides"] = inst->overrides;
        out["members"] = inst->members;
        out["root_template"] = inst->root_template_uuid;

        nlohmann::json children = nlohmann::json::array();
        for (const auto& child : entity.children()) {
            if (!child->prefab_template_uuid().empty()) continue; // 模板成员跳过
            children.push_back(serialize_entity(*child));
        }
        out["children"] = std::move(children);
        return out;
    }

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
        auto entity = deserialize_entity(e_json, entity_map);
        if (!entity) continue;

        if (e_json.contains("parent") && !e_json["parent"].is_null()) {
            parent_map[entity->uuid()] = UUID(e_json["parent"].get<std::string>());
        }
        all_entities.push_back(std::move(entity));
    }

    // 重建父子关系
    for (const auto& [child_id, parent_id] : parent_map) {
        auto child_it = entity_map.find(child_id);
        auto parent_it = entity_map.find(parent_id);
        if (child_it == entity_map.end() || parent_it == entity_map.end()) continue;

        // 防自父/父子环：恶意 .gesc 可构造环形层级，
        // 导致 foreach/析构无限递归栈溢出。检测到则拒绝该父子关系。
        if (child_id == parent_id) {
            GLOG_WARN("SceneSerializer: entity '{}' has itself as parent, ignoring", child_id.str());
            continue;
        }
        bool has_cycle = false;
        {
            // 沿 parent 链向上走，若遇到 child 则成环；visited 防御数据中已有的其他环
            std::unordered_set<UUID> visited;
            UUID cur = parent_id;
            while (true) {
                auto next = parent_map.find(cur);
                if (next == parent_map.end()) break;
                if (next->second == child_id) { has_cycle = true; break; }
                if (!visited.insert(next->second).second) break;
                cur = next->second;
            }
        }
        if (has_cycle) {
            GLOG_WARN("SceneSerializer: parenting '{}' -> '{}' would create a cycle, ignoring",
                      child_id.str(), parent_id.str());
            continue;
        }

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

std::unique_ptr<Entity> SceneSerializer::deserialize_entity(
    const nlohmann::json& e_json,
    std::unordered_map<UUID, Entity*>& entity_map) {

    // Prefab 引用节点：实例化 -> 应用覆盖 -> 回填保存的成员 UUID
    if (e_json.contains("prefab") && e_json["prefab"].is_string()) {
        auto root = Prefab::instantiate_tree(e_json["prefab"].get<std::string>(),
                                             e_json.value("overrides", nlohmann::json::object()));
        if (!root) {
            GLOG_ERROR("SceneSerializer: failed to instantiate prefab '{}'",
                       e_json["prefab"].get<std::string>());
            return nullptr;
        }
        root->set_uuid(UUID(e_json.value("uuid", UUID::generate().str())));
        if (e_json.contains("name") && e_json["name"].is_string()) {
            root->set_name(e_json["name"].get<std::string>());
        }
        root->enabled = e_json.value("enabled", true);

        // 按保存的 members 映射回填成员 UUID，保持场景内引用稳定
        if (auto* inst = Prefab::get_instance(root.get())) {
            const auto saved = e_json.value("members", nlohmann::json::object());
            if (saved.is_object()) {
                Entity* root_raw = root.get();
                root->foreach([&](Entity* e) {
                    const std::string tpl = (e == root_raw)
                        ? inst->root_template_uuid
                        : e->prefab_template_uuid();
                    if (tpl.empty()) return;
                    auto it = saved.find(tpl);
                    if (it != saved.end() && it->is_string()) {
                        e->set_uuid(UUID(it->get<std::string>()));
                    }
                });
                // 实例根 UUID 以节点 "uuid" 字段为准
                root->set_uuid(UUID(e_json.value("uuid", root->uuid().str())));
            }
            Prefab::refresh_members(root.get());
        }

        // 注册整棵实例子树（模板成员也可能被扁平列表中的实体引用）
        root->foreach([&](Entity* e) { entity_map[e->uuid()] = e; });

        // 运行时添加的子实体（嵌套序列化在 "children" 里）
        for (const auto& child_json : e_json.value("children", nlohmann::json::array())) {
            auto child = deserialize_entity(child_json, entity_map);
            if (child) {
                root->add_child(std::move(child));
            }
        }
        return root;
    }

    // 普通实体节点
    UUID id(e_json.value("uuid", UUID::generate().str()));
    auto entity = std::make_unique<Entity>(e_json.value("name", "Entity"));
    entity->set_uuid(id);
    entity->enabled = e_json.value("enabled", true);

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

    entity_map[id] = entity.get();

    // 嵌套子实体（Prefab 实例的 "children" 之外的场景不产出此结构，递归兜底）
    for (const auto& child_json : e_json.value("children", nlohmann::json::array())) {
        auto child = deserialize_entity(child_json, entity_map);
        if (child) {
            entity->add_child(std::move(child));
        }
    }

    return entity;
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
