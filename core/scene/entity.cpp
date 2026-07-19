#include "entity.h"

#include "components/component_factory.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

ecs::EntityID Entity::generate_id() {
    static ecs::EntityID next = 1;
    return next++;
}

Entity::Entity(const std::string& name)
    : name_(name)
    , uuid_(UUID::generate())
    , id_(generate_id()) {
    // 每个 Entity 默认带 Transform；store_ 由 Scene 在创建后设置，
    // 因此 Transform 的注册会延迟到 set_store() 中处理。
    transform_ = static_cast<components::Transform*>(
        add_component(std::make_unique<components::Transform>()));
}

void Entity::set_store(ecs::ComponentStore* store) {
    if (store_ == store) return;
    if (store_) {
        store_->unregister_entity(id_);
    }
    store_ = store;
    if (store_) {
        for (const auto& comp : components_) {
            store_->register_component(id_, std::type_index(typeid(*comp)), comp.get());
        }
    }
}

Entity::~Entity() {
    on_destroy();
    if (store_) {
        store_->unregister_entity(id_);
    }
}

components::Component* Entity::add_component(std::unique_ptr<components::Component> comp) {
    if (!comp) return nullptr;
    components::Component* raw = comp.get();
    raw->on_attach(this);
    components_.push_back(std::move(comp));
    if (store_) {
        store_->register_component(id_, std::type_index(typeid(*raw)), raw);
    }
    // 生命周期：awake（组件已挂载但场景尚未开始）
    raw->on_awake();
    mark_dirty();
    return raw;
}

bool Entity::remove_component(components::Component* comp) {
    if (!comp) return false;
    for (auto it = components_.begin(); it != components_.end(); ++it) {
        if (it->get() == comp) {
            if (store_) {
                store_->unregister_component(id_, std::type_index(typeid(*comp)));
            }
            comp->on_disable();
            comp->on_detach();
            components_.erase(it);
            mark_dirty();
            return true;
        }
    }
    return false;
}

void Entity::on_init() {
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_init();
    }
    for (auto& child : children_) {
        child->on_init();
    }
}

void Entity::on_start() {
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_start();
    }
    for (auto& child : children_) {
        child->on_start();
    }
}

void Entity::on_enable() {
    if (!enabled) return;
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_enable();
    }
    for (auto& child : children_) {
        child->on_enable();
    }
}

void Entity::on_disable() {
    for (auto& comp : components_) {
        comp->on_disable();
    }
    for (auto& child : children_) {
        child->on_disable();
    }
}

void Entity::on_update(float dt) {
    if (!enabled) return;
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_update(dt);
    }
    for (auto& child : children_) {
        child->on_update(dt);
    }
}

void Entity::on_render(render::RenderContext& ctx) {
    if (!enabled) return;
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_render(ctx);
    }
    for (auto& child : children_) {
        child->on_render(ctx);
    }
}

void Entity::on_destroy() {
    // 幂等保护：层级实体的 on_destroy 可能被调两次
    // （Scene::destroy() 显式调用一次，之后 ~Entity 析构链再调一次），
    // 用标志位保证只生效一次。与 AudioSource 等组件的幂等实现兼容。
    if (destroy_notified_) return;
    destroy_notified_ = true;
    for (auto& comp : components_) {
        comp->on_destroy();
    }
    for (auto& child : children_) {
        child->on_destroy();
    }
}

void Entity::set_parent(Entity* parent) {
    if (parent_ == parent) return;

    // 注意：parent_->remove_child(this) 会 erase 持有 this 的 unique_ptr，
    // 可能导致本对象立即析构（use-after-free 隐患）。
    // 因此所有成员写入必须在 remove_child 之前完成，
    // remove_child 之后不得再访问 this，直接返回。
    // 需要“重挂父级”的正确路径是 detach_child() + add_child()。
    Entity* old_parent = parent_;
    parent_ = parent;
    if (old_parent) {
        old_parent->remove_child(this); // 此后 this 可能已析构
        return;
    }
    // 无旧父级（根实体）：仅做指针链接，不转移所有权
}

Entity* Entity::add_child(std::unique_ptr<Entity> child) {
    if (!child) return nullptr;

    Entity* raw = child.get();
    raw->parent_ = this;
    // 子实体继承父实体的组件存储池
    if (store_) {
        raw->set_store(store_);
    }
    children_.push_back(std::move(child));
    return raw;
}

bool Entity::remove_child(Entity* child) {
    if (!child) return false;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            child->parent_ = nullptr;
            children_.erase(it);
            return true;
        }
    }
    return false;
}

std::unique_ptr<Entity> Entity::detach_child(Entity* child) {
    if (!child) return nullptr;
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<Entity> owned = std::move(*it);
            children_.erase(it);
            owned->parent_ = nullptr;
            return owned;
        }
    }
    return nullptr;
}

components::Component* Entity::get_component_by_type(const std::string& type) const {
    for (const auto& comp : components_) {
        if (type == comp->type()) {
            return comp.get();
        }
    }
    return nullptr;
}

math::Matrix4f Entity::world_transform() const {
    if (parent_) {
        return parent_->world_transform() * transform_->local_matrix();
    }
    return transform_->local_matrix();
}

void Entity::foreach(std::function<void(Entity*)> callback) {
    callback(this);
    for (auto& child : children_) {
        child->foreach(callback);
    }
}

std::unique_ptr<Entity> Entity::clone() const {
    auto clone_entity = std::make_unique<Entity>(name_);
    clone_entity->enabled = enabled;
    clone_entity->prefab_template_uuid_ = prefab_template_uuid_;

    // 深拷贝所有组件（除 Transform 外，Transform 由构造函数自动创建）
    for (const auto& comp : components_) {
        if (comp->type() == std::string("Transform")) {
            // 深拷贝 Transform 数据
            nlohmann::json t_json;
            comp->serialize(t_json);
            if (clone_entity->transform_) {
                clone_entity->transform_->deserialize(t_json);
            }
            continue;
        }
        auto new_comp = components::ComponentFactory::instance().create(comp->type());
        if (!new_comp) continue;
        nlohmann::json json;
        comp->serialize(json);
        new_comp->deserialize(json);
        new_comp->enabled = comp->enabled;
        clone_entity->add_component(std::move(new_comp));
    }

    // 递归深拷贝子节点
    for (const auto& child : children_) {
        auto child_clone = child->clone();
        clone_entity->add_child(std::move(child_clone));
    }

    return clone_entity;
}

nlohmann::json Entity::snapshot_runtime_state() const {
    nlohmann::json out;
    out["uuid"] = uuid_.str();
    out["enabled"] = enabled;

    nlohmann::json comps = nlohmann::json::array();
    for (const auto& comp : components_) {
        if (comp->type() == std::string("Transform")) continue;
        nlohmann::json c_json;
        c_json["type"] = comp->type();
        comp->snapshot_runtime_state(c_json);
        if (!c_json.empty()) {
            comps.push_back(c_json);
        }
    }
    if (!comps.empty()) {
        out["components"] = comps;
    }
    return out;
}

void Entity::restore_runtime_state(const nlohmann::json& json) {
    enabled = json.value("enabled", true);
    for (const auto& c_json : json.value("components", nlohmann::json::array())) {
        std::string type = c_json.value("type", "");
        if (type.empty()) continue;
        auto* comp = get_component_by_type(type);
        if (comp) {
            comp->restore_runtime_state(c_json);
        }
    }
}

} // namespace gryce_engine::scene
