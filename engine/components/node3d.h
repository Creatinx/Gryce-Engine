#pragma once

#include "components/node.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Node3D — 标记实体为 3D 节点（Godot 风格）。
// 3D 节点的变换、层级由 Entity + Transform 提供；本组件补充 3D 可见性等属性。
// ---------------------------------------------------------------------------
class Node3D : public Node {
public:
    Node3D() = default;

    NodeDomain node_domain() const noexcept override { return NodeDomain::Scene3D; }

    const char* type() const override { return "Node3D"; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        Node::collect_type_views(out);
        out.push_back({std::type_index(typeid(Node3D)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    void serialize(nlohmann::json& out) const override {
        serialize_node_base(out);
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node_base(in);
    }
};

} // namespace gryce_engine::components
