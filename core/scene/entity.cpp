#include "entity.h"

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

void Entity::on_init() {
    for (auto& comp : components_) {
        if (comp->enabled) comp->on_init();
    }
    for (auto& child : children_) {
        child->on_init();
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
    for (auto& comp : components_) {
        comp->on_destroy();
    }
    for (auto& child : children_) {
        child->on_destroy();
    }
}

void Entity::set_parent(Entity* parent) {
    if (parent_ == parent) return;

    // 从旧父级移除
    if (parent_) {
        parent_->remove_child(this);
    }

    parent_ = parent;

    // 加入新父级
    if (parent_) {
        // 注意：这里假设当前 Entity 已经由某个 unique_ptr 管理，
        // 不转移所有权，仅做指针链接。实际使用建议通过 add_child 建立关系。
    }
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

components::Component* Entity::add_component(std::unique_ptr<components::Component> comp) {
    if (!comp) return nullptr;
    components::Component* raw = comp.get();
    raw->on_attach(this);
    components_.push_back(std::move(comp));
    if (store_) {
        store_->register_component(id_, std::type_index(typeid(*raw)), raw);
    }
    return raw;
}

bool Entity::remove_component(components::Component* comp) {
    if (!comp) return false;
    for (auto it = components_.begin(); it != components_.end(); ++it) {
        if (it->get() == comp) {
            if (store_) {
                store_->unregister_component(id_, std::type_index(typeid(*comp)));
            }
            comp->on_detach();
            components_.erase(it);
            return true;
        }
    }
    return false;
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

} // namespace gryce_engine::scene
