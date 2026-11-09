#pragma once

#include <cstdint>

#include "components/component.h"
#include "export.h"

namespace gryce_engine::components {

// 节点所属的运行/渲染域。Node 是场景中对象的身份，不等同于可绘制组件：
// 一个 2D Entity 通常挂一个 Node2D，再按需挂 Sprite2D / Label / Collider2D。
enum class NodeDomain : uint8_t {
    Scene3D = 0,
    Canvas2D = 1,
    UI = 2,
};

// ---------------------------------------------------------------------------
// Node — Node3D / Node2D / NodeUI 的公共基类
//
// 变换与 Entity 父子关系仍由 Entity::Transform 提供，Node 只负责节点身份和
// 节点级可见性。这样不会出现“组件自带一份 Transform，Entity 又自带一份”
// 的双重真源。
// ---------------------------------------------------------------------------
class GRYCE_API Node : public Component {
public:
    ~Node() override = default;

    virtual NodeDomain node_domain() const noexcept = 0;

    bool visible = true;

    // 同时考虑：
    //   - 本组件的 enabled / visible；
    //   - Node / NodeUI 组件树上的父组件；
    //   - Entity 祖先链上的 enabled 与节点可见性。
    virtual bool is_visible_in_tree() const;

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        Component::collect_type_views(out);
        out.push_back({std::type_index(typeid(Node)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

protected:
    Node() = default;

    void serialize_node_base(nlohmann::json& out) const {
        out["visible"] = visible;
    }

    void deserialize_node_base(const nlohmann::json& in) {
        visible = in.value("visible", true);
    }
};

} // namespace gryce_engine::components
