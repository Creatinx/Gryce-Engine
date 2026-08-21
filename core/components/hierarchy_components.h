#pragma once

#include "components/component.h"
#include "ecs/types.h"

#include <vector>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// ParentComponent — 存储父实体的 ECS ID
// 每个 Entity 自动拥有此组件（除非是孤儿根节点）。
// 使 ECS 系统可按父级过滤查询所有子实体。
// ---------------------------------------------------------------------------
class GRYCE_API ParentComponent : public Component {
public:
    ecs::EntityID parent_id = ecs::k_invalid_entity;

    ParentComponent() = default;
    explicit ParentComponent(ecs::EntityID pid) : parent_id(pid) {}

    const char* type() const override { return "ParentComponent"; }

    void serialize(nlohmann::json& out) const override {
        out["parent_id"] = static_cast<uint64_t>(parent_id);
    }

    void deserialize(const nlohmann::json& in) override {
        parent_id = static_cast<ecs::EntityID>(in.value("parent_id", 0ULL));
    }
};

// ---------------------------------------------------------------------------
// ChildrenComponent — 存储子实体的 ECS ID 列表
// 每个 Entity 自动拥有此组件（可能为空列表）。
// 使 ECS 系统可按子级关系遍历实体树，无需依赖 Entity 的 unique_ptr 树。
// ---------------------------------------------------------------------------
class GRYCE_API ChildrenComponent : public Component {
public:
    std::vector<ecs::EntityID> child_ids;

    ChildrenComponent() = default;

    const char* type() const override { return "ChildrenComponent"; }

    void serialize(nlohmann::json& out) const override {
        nlohmann::json arr = nlohmann::json::array();
        for (auto id : child_ids) {
            arr.push_back(static_cast<uint64_t>(id));
        }
        out["child_ids"] = std::move(arr);
    }

    void deserialize(const nlohmann::json& in) override {
        child_ids.clear();
        for (const auto& v : in.value("child_ids", nlohmann::json::array())) {
            child_ids.push_back(static_cast<ecs::EntityID>(v.get<uint64_t>()));
        }
    }
};

} // namespace gryce_engine::components