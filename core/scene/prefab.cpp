#include "prefab.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "scene.h"
#include "scene_serializer.h"
#include "components/component_factory.h"
#include "components/prefab_instance.h"
#include "components/transform.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

namespace {

// 嵌套实例化的循环引用检测（实例化栈：outer -> inner -> ...）
thread_local std::vector<std::string> g_instantiate_stack;

// 模板缓存（强引用，避免每次实例化都重新读盘；clear_cache() 可整体失效）
std::unordered_map<std::string, std::shared_ptr<Prefab>>& prefab_cache() {
    static std::unordered_map<std::string, std::shared_ptr<Prefab>> cache;
    return cache;
}

struct InstantiateGuard {
    explicit InstantiateGuard(std::string path) : path_(std::move(path)) {
        g_instantiate_stack.push_back(path_);
    }
    ~InstantiateGuard() {
        if (!g_instantiate_stack.empty() && g_instantiate_stack.back() == path_) {
            g_instantiate_stack.pop_back();
        }
    }
    std::string path_;
};

// 并行遍历模板树与实例树，记录 模板UUID -> 实例UUID，并给非根成员打模板标记
void mark_members(const Entity* tpl, Entity* inst, bool is_root, nlohmann::json& members) {
    members[tpl->uuid().str()] = inst->uuid().str();
    if (!is_root) {
        inst->set_prefab_template_uuid(tpl->uuid().str());
    }
    const auto& tpl_children = tpl->children();
    const auto& inst_children = inst->children();
    const size_t count = std::min(tpl_children.size(), inst_children.size());
    for (size_t i = 0; i < count; ++i) {
        mark_members(tpl_children[i].get(), inst_children[i].get(), false, members);
    }
}

// 组件深合并覆盖：序列化当前状态 -> json::update -> 反序列化回去
void merge_json_into_component(components::Component* comp, const nlohmann::json& patch) {
    if (!comp || !patch.is_object()) return;
    nlohmann::json base;
    comp->serialize(base);
    base.update(patch);
    comp->deserialize(base);
    if (patch.contains("enabled") && patch["enabled"].is_boolean()) {
        comp->enabled = patch["enabled"].get<bool>();
    }
}

void propagate_store(Entity* entity, ecs::ComponentStore* store) {
    entity->set_store(store);
    for (const auto& child : entity->children()) {
        propagate_store(child.get(), store);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 模板加载
// ---------------------------------------------------------------------------
std::unique_ptr<Prefab> Prefab::load(const std::string& path) {
    auto scene = SceneSerializer::load_from_file(path);
    if (!scene) {
        GLOG_ERROR("Prefab::load: failed to load prefab from '{}'", path);
        return nullptr;
    }

    // 模板实体脱离临时 Scene 的 ComponentStore（Scene 析构后 store 指针会悬空）
    scene->foreach([](Entity* e) { e->set_store(nullptr); });

    auto prefab = std::make_unique<Prefab>();
    // 从加载的 Scene 中移出所有根实体（不触发 Scene 析构时的组件反注册）
    auto& roots = scene->roots();
    for (auto& root : roots) {
        prefab->roots_.push_back(std::move(root));
    }
    roots.clear(); // 清空原 vector，避免 scene 析构时重复释放

    return prefab;
}

std::shared_ptr<Prefab> Prefab::load_cached(const std::string& path) {
    const std::string key = resources::ResourcePath::resolve(path);
    auto& cache = prefab_cache();

    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    auto prefab = load(path);
    if (!prefab) return nullptr;
    std::shared_ptr<Prefab> shared(std::move(prefab));
    cache[key] = shared;
    return shared;
}

void Prefab::clear_cache() {
    prefab_cache().clear();
}

// ---------------------------------------------------------------------------
// 兼容旧 API：纯拷贝式实例化
// ---------------------------------------------------------------------------
Entity* Prefab::instantiate(Scene* scene) const {
    if (!scene || roots_.empty()) return nullptr;

    Entity* first_root = nullptr;
    for (const auto& root : roots_) {
        auto cloned = root->clone();
        Entity* raw = cloned.get();
        if (!first_root) first_root = raw;
        scene->add_root_entity(std::move(cloned));
    }
    return first_root;
}

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------
bool Prefab::save(const Entity* root, const std::string& path) {
    if (!root) return false;

    nlohmann::json out;
    out["version"] = 1;
    out["type"] = "prefab";
    out["name"] = root->name();

    nlohmann::json entities = nlohmann::json::array();
    bool first = true;
    const_cast<Entity*>(root)->foreach([&](Entity* e) {
        if (e != root) {
            // 嵌套实例的模板成员 / 其实例节点 "children" 携带的实体不重复序列化
            if (!e->prefab_template_uuid().empty()) return;
            for (Entity* p = e->parent(); p != nullptr; p = p->parent()) {
                if (Prefab::get_instance(p) != nullptr) return;
            }
        }
        auto e_json = SceneSerializer::serialize_entity(*e);
        if (first) {
            e_json["parent"] = nullptr; // 截断 root 的外部父引用
            first = false;
        }
        entities.push_back(std::move(e_json));
    });
    out["entities"] = std::move(entities);

    const std::string resolved = resources::ResourcePath::resolve(path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(resolved).parent_path(), ec);
    std::ofstream file(resolved);
    if (!file) {
        GLOG_ERROR("Prefab::save: failed to write '{}'", resolved);
        return false;
    }
    file << out.dump(2);
    if (!file.good()) return false;
    GLOG_INFO("Prefab saved to '{}'", resolved);
    return true;
}

// ---------------------------------------------------------------------------
// 实例化
// ---------------------------------------------------------------------------
std::unique_ptr<Entity> Prefab::instantiate_tree(const std::string& prefab_path,
                                                 const nlohmann::json& overrides) {
    const std::string resolved = resources::ResourcePath::resolve(prefab_path);

    // 处理 Prefab Variant 文件：它只保存 base prefab 路径与覆盖参数
    std::string variant_of_path;
    std::string base_prefab_path = prefab_path;
    nlohmann::json variant_overrides = overrides.is_object() ? overrides : nlohmann::json::object();

    if (resolved.ends_with(".geprefabvariant")) {
        std::ifstream ifs(resolved);
        if (!ifs) {
            GLOG_ERROR("Prefab::instantiate_tree: failed to open variant '{}'", prefab_path);
            return nullptr;
        }
        try {
            nlohmann::json variant_json = nlohmann::json::parse(ifs);
            std::string base = variant_json.value("base_prefab", "");
            if (base.empty()) {
                GLOG_ERROR("Prefab::instantiate_tree: variant '{}' missing base_prefab", prefab_path);
                return nullptr;
            }
            variant_of_path = prefab_path;
            base_prefab_path = base;
            if (variant_json.contains("overrides") && variant_json["overrides"].is_object()) {
                // 调用方传入的 overrides 优先级高于 variant 文件内的 overrides
                nlohmann::json file_overrides = variant_json["overrides"];
                file_overrides.update(variant_overrides);
                variant_overrides = std::move(file_overrides);
            }
        } catch (const std::exception& e) {
            GLOG_ERROR("Prefab::instantiate_tree: failed to parse variant '{}': {}", prefab_path, e.what());
            return nullptr;
        }
    }

    // 循环引用检测（基于最终解析的 base prefab）
    const std::string base_resolved = resources::ResourcePath::resolve(base_prefab_path);
    for (const auto& active : g_instantiate_stack) {
        if (active == base_resolved) {
            GLOG_ERROR("Prefab: cyclic prefab reference detected at '{}'", base_prefab_path);
            return nullptr;
        }
    }
    InstantiateGuard guard(base_resolved);

    auto prefab = load_cached(base_prefab_path);
    if (!prefab || prefab->roots_.empty()) {
        GLOG_ERROR("Prefab::instantiate_tree: failed to load '{}'", base_prefab_path);
        return nullptr;
    }

    // 构建实例根：单根直接克隆；多根包一层容器实体
    std::unique_ptr<Entity> root;
    nlohmann::json members = nlohmann::json::object();
    std::string root_template_uuid;

    if (prefab->roots_.size() == 1) {
        root = prefab->roots_[0]->clone();
        root_template_uuid = prefab->roots_[0]->uuid().str();
        mark_members(prefab->roots_[0].get(), root.get(), true, members);
    } else {
        root = std::make_unique<Entity>(std::filesystem::path(resolved).stem().string());
        for (const auto& tpl_root : prefab->roots_) {
            auto cloned = tpl_root->clone();
            mark_members(tpl_root.get(), cloned.get(), false, members);
            root->add_child(std::move(cloned));
        }
    }

    // 挂实例标记组件
    auto instance = std::make_unique<components::PrefabInstance>(prefab_path);
    instance->overrides = variant_overrides.is_object() ? variant_overrides : nlohmann::json::object();
    instance->members = std::move(members);
    instance->root_template_uuid = std::move(root_template_uuid);
    instance->variant_of = std::move(variant_of_path);
    root->add_component(std::move(instance));

    // 应用覆盖参数
    if (variant_overrides.is_object() && !variant_overrides.empty()) {
        apply_overrides(root.get(), variant_overrides);
    }

    return root;
}

Entity* Prefab::instantiate(Scene* scene, const std::string& prefab_path,
                            const nlohmann::json& overrides) {
    if (!scene) return nullptr;
    auto tree = instantiate_tree(prefab_path, overrides);
    if (!tree) return nullptr;
    return scene->add_root_entity(std::move(tree));
}

// ---------------------------------------------------------------------------
// 覆盖参数
// ---------------------------------------------------------------------------
components::PrefabInstance* Prefab::get_instance(Entity* entity) {
    if (!entity) return nullptr;
    return dynamic_cast<components::PrefabInstance*>(
        entity->get_component_by_type("PrefabInstance"));
}

const components::PrefabInstance* Prefab::get_instance(const Entity* entity) {
    if (!entity) return nullptr;
    return dynamic_cast<const components::PrefabInstance*>(
        entity->get_component_by_type("PrefabInstance"));
}

namespace {

// 在实例子树中按 模板UUID / 实例UUID / 名称 定位成员
Entity* resolve_member(Entity* root, components::PrefabInstance* inst, const std::string& key) {
    if (!root || key.empty()) return nullptr;

    // 1) members 映射：模板UUID -> 实例UUID
    if (inst) {
        std::string instance_uuid = inst->find_instance_uuid(key);
        if (!instance_uuid.empty()) {
            Entity* found = nullptr;
            root->foreach([&](Entity* e) {
                if (!found && e->uuid().str() == instance_uuid) found = e;
            });
            if (found) return found;
        }
        // 根模板 UUID 直接命中
        if (key == inst->root_template_uuid) return root;
    }

    // 2) 模板成员标记直配
    Entity* found = nullptr;
    root->foreach([&](Entity* e) {
        if (!found && e->prefab_template_uuid() == key) found = e;
    });
    if (found) return found;

    // 3) 实例 UUID
    root->foreach([&](Entity* e) {
        if (!found && e->uuid().str() == key) found = e;
    });
    if (found) return found;

    // 4) 名称（子树内第一个匹配）
    root->foreach([&](Entity* e) {
        if (!found && e->name() == key) found = e;
    });
    return found;
}

void apply_entity_overrides(Entity* entity, const nlohmann::json& ov) {
    if (!entity || !ov.is_object()) return;

    if (ov.contains("name") && ov["name"].is_string()) {
        entity->set_name(ov["name"].get<std::string>());
    }
    if (ov.contains("enabled") && ov["enabled"].is_boolean()) {
        entity->enabled = ov["enabled"].get<bool>();
    }
    if (ov.contains("transform") && ov["transform"].is_object()) {
        if (auto* t = entity->transform()) {
            merge_json_into_component(t, ov["transform"]);
        }
    }
    if (ov.contains("components") && ov["components"].is_object()) {
        for (const auto& [type, patch] : ov["components"].items()) {
            components::Component* comp = entity->get_component_by_type(type);
            if (!comp) {
                // 覆盖允许新增组件
                auto created = components::ComponentFactory::instance().create(type);
                if (created) {
                    comp = entity->add_component(std::move(created));
                }
            }
            if (comp) {
                merge_json_into_component(comp, patch);
            } else {
                GLOG_WARN("Prefab override: unknown component type '{}' on '{}'", type, entity->name());
            }
        }
    }
}

} // namespace

void Prefab::apply_overrides(Entity* instance_root, const nlohmann::json& overrides) {
    if (!instance_root || !overrides.is_object()) return;
    auto* inst = get_instance(instance_root);

    // 根级覆盖
    apply_entity_overrides(instance_root, overrides);

    // 子实体覆盖
    if (overrides.contains("entities") && overrides["entities"].is_object()) {
        for (const auto& [key, ent_ov] : overrides["entities"].items()) {
            Entity* target = resolve_member(instance_root, inst, key);
            if (target) {
                apply_entity_overrides(target, ent_ov);
            } else {
                GLOG_WARN("Prefab override: member '{}' not found in '{}'", key, instance_root->name());
            }
        }
    }

    // 移除模板成员
    if (overrides.contains("remove") && overrides["remove"].is_array()) {
        for (const auto& item : overrides["remove"]) {
            if (!item.is_string()) continue;
            Entity* target = resolve_member(instance_root, inst, item.get<std::string>());
            if (target && target != instance_root && target->parent()) {
                target->parent()->remove_child(target); // 销毁子树
            }
        }
    }
}

void Prefab::refresh_members(Entity* instance_root) {
    auto* inst = get_instance(instance_root);
    if (!inst) return;

    nlohmann::json members = nlohmann::json::object();
    if (!inst->root_template_uuid.empty()) {
        members[inst->root_template_uuid] = instance_root->uuid().str();
    }
    instance_root->foreach([&](Entity* e) {
        if (e == instance_root) return;
        if (!e->prefab_template_uuid().empty()) {
            members[e->prefab_template_uuid()] = e->uuid().str();
        }
    });
    inst->members = std::move(members);
}

// ---------------------------------------------------------------------------
// 还原实例
// ---------------------------------------------------------------------------
bool Prefab::revert(Entity* instance_root) {
    auto* inst = get_instance(instance_root);
    if (!inst) return false;

    refresh_members(instance_root);

    // 1) 暂存运行时添加的子实体（非模板成员）
    std::vector<std::unique_ptr<Entity>> added;
    {
        std::vector<Entity*> ptrs;
        for (const auto& child : instance_root->children()) {
            if (!inst->is_template_member(child->uuid().str())) {
                ptrs.push_back(child.get());
            }
        }
        for (Entity* p : ptrs) {
            added.push_back(instance_root->detach_child(p));
        }
    }

    // 2) 销毁剩余的模板成员子树
    {
        std::vector<Entity*> ptrs;
        for (const auto& child : instance_root->children()) {
            ptrs.push_back(child.get());
        }
        for (Entity* p : ptrs) {
            instance_root->remove_child(p);
        }
    }

    // 3) 按模板+覆盖重建
    auto fresh = instantiate_tree(inst->prefab_path, inst->overrides);
    if (!fresh) {
        GLOG_ERROR("Prefab::revert: failed to re-instantiate '{}'", inst->prefab_path);
        for (auto& a : added) {
            instance_root->add_child(std::move(a));
        }
        return false;
    }
    auto* fresh_inst = get_instance(fresh.get());

    // 根组件替换（保留 Transform 与 PrefabInstance 本身）
    {
        std::vector<components::Component*> to_remove;
        for (const auto& comp : instance_root->components()) {
            const std::string type = comp->type();
            if (type != "Transform" && type != "PrefabInstance") {
                to_remove.push_back(comp.get());
            }
        }
        for (auto* comp : to_remove) {
            instance_root->remove_component(comp);
        }
        for (const auto& comp : fresh->components()) {
            const std::string type = comp->type();
            if (type == "Transform") {
                nlohmann::json t_json;
                comp->serialize(t_json);
                instance_root->transform()->deserialize(t_json);
            } else if (type != "PrefabInstance") {
                auto new_comp = components::ComponentFactory::instance().create(type);
                if (!new_comp) continue;
                nlohmann::json c_json;
                comp->serialize(c_json);
                new_comp->deserialize(c_json);
                new_comp->enabled = comp->enabled;
                instance_root->add_component(std::move(new_comp));
            }
        }
        instance_root->enabled = fresh->enabled;
        instance_root->set_name(fresh->name());
    }

    // 4) 成员沿用旧实例 UUID，保持场景内引用稳定
    {
        const nlohmann::json& old_members = inst->members;
        fresh->foreach([&](Entity* e) {
            const std::string tpl = (e == fresh.get())
                ? (fresh_inst ? fresh_inst->root_template_uuid : "")
                : e->prefab_template_uuid();
            if (tpl.empty()) return;
            auto it = old_members.find(tpl);
            if (it != old_members.end() && it->is_string()) {
                e->set_uuid(UUID(it->get<std::string>()));
            }
        });
    }

    // 5) 搬移模板子树
    while (!fresh->children().empty()) {
        auto child = fresh->detach_child(fresh->children().front().get());
        Entity* raw = child.get();
        instance_root->add_child(std::move(child));
        if (instance_root->store()) {
            propagate_store(raw, instance_root->store());
        }
    }

    // 6) 挂回运行时添加的子实体
    for (auto& a : added) {
        instance_root->add_child(std::move(a));
    }

    // 7) 更新实例元数据
    if (fresh_inst) {
        inst->root_template_uuid = fresh_inst->root_template_uuid;
    }
    refresh_members(instance_root);
    instance_root->mark_dirty();
    return true;
}

} // namespace gryce_engine::scene
