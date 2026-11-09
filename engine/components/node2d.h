#pragma once

#include "components/node.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Node2D — 2D 场景对象的节点身份。
// 2D 节点的变换、层级仍由 Entity + Transform 提供；本组件补充 2D 专属属性。
// ---------------------------------------------------------------------------
class Node2D : public Node {
public:
    // 绘制/逻辑层级，数值越大越靠上（后处理）。
    int z_index = 0;

    // 若为 true，则忽略父级 2D 变换影响（Godot 的 top_level）。
    bool top_level = false;

    Node2D() = default;

    NodeDomain node_domain() const noexcept override { return NodeDomain::Canvas2D; }

    const char* type() const override { return "Node2D"; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        Node::collect_type_views(out);
        out.push_back({std::type_index(typeid(Node2D)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    void serialize(nlohmann::json& out) const override {
        serialize_node2d_base(out);
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node2d_base(in);
    }

protected:
    void serialize_node2d_base(nlohmann::json& out) const {
        serialize_node_base(out);
        out["z_index"] = z_index;
        out["top_level"] = top_level;
    }

    void deserialize_node2d_base(const nlohmann::json& in) {
        deserialize_node_base(in);
        z_index = in.value("z_index", 0);
        top_level = in.value("top_level", false);
    }
};

} // namespace gryce_engine::components
