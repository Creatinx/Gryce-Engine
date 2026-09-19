#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// PrefabInstance — 标记实体为某个 Prefab 的实例根。
//
// - prefab_path：源预制体文件（.gesc / .geprefab，支持 res:/ 前缀）
// - overrides ：实例覆盖参数（JSON 对象，结构见 scene/prefab.h 文档）
// - members   ：模板UUID -> 实例UUID 映射（实例化时生成，用于序列化与覆盖定位）
//
// 场景序列化时，带此组件的实体会写成紧凑形式：
//   { "prefab": path, "overrides": {...}, "members": {...}, "children": [...] }
// 加载时重新实例化并应用覆盖，预制体文件的修改会自动传播到所有场景。
// ---------------------------------------------------------------------------
class PrefabInstance : public Component {
public:
    std::string prefab_path;
    nlohmann::json overrides = nlohmann::json::object();
    nlohmann::json members = nlohmann::json::object();
    std::string root_template_uuid; // 实例根在预制体文件中的模板 UUID
    std::string variant_of;         // 若来源于 .geprefabvariant 文件，记录 variant 文件路径

    PrefabInstance() = default;
    explicit PrefabInstance(const std::string& path) : prefab_path(path) {}

    const char* type() const override { return "PrefabInstance"; }

    void serialize(nlohmann::json& out) const override {
        out["prefab"] = prefab_path;
        out["overrides"] = overrides;
        out["members"] = members;
        out["root_template"] = root_template_uuid;
        out["variant_of"] = variant_of;
    }

    void deserialize(const nlohmann::json& in) override {
        prefab_path = in.value("prefab", "");
        overrides = in.value("overrides", nlohmann::json::object());
        members = in.value("members", nlohmann::json::object());
        root_template_uuid = in.value("root_template", "");
        variant_of = in.value("variant_of", "");
        if (overrides.is_null()) overrides = nlohmann::json::object();
        if (members.is_null()) members = nlohmann::json::object();
    }

    // 判断给定实例 UUID 是否为模板成员（相对独立添加的实体）
    bool is_template_member(const std::string& instance_uuid) const {
        for (const auto& [key, value] : members.items()) {
            (void)key;
            if (value.is_string() && value.get<std::string>() == instance_uuid) return true;
        }
        return false;
    }

    // 按模板 UUID 查实例 UUID（找不到返回空串）
    std::string find_instance_uuid(const std::string& template_uuid) const {
        auto it = members.find(template_uuid);
        if (it != members.end() && it->is_string()) return it->get<std::string>();
        return "";
    }
};

} // namespace gryce_engine::components
