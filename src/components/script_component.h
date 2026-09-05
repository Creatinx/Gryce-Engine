#pragma once

#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"

namespace gryce_engine::components {

/// Exposed script property (from the script's `props` table). type:
/// 0 = float, 1 = string. Values stay in sync with the script environment and are
/// serialized with the scene.
struct GRYCE_API ScriptProp {
    std::string name;
    int type = 0;
    float f = 0.0f;
    std::string s;
};

/// ScriptComponent: binds a .js file to an entity. The JS
/// callbacks (on_start / on_update / on_destroy) are driven by ScriptSystem.
/// Only script_path + enabled are serialized; the module namespace is runtime
/// state managed by ScriptSystem.
class GRYCE_API ScriptComponent : public Component {
public:
    std::string script_path;

    ScriptComponent() = default;

    const char* type() const override { return "Script"; }

    void serialize(nlohmann::json& out) const override {
        out["script_path"] = script_path;
        if (!props.empty()) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : props) {
                arr.push_back({
                    {"name", p.name},
                    {"type", p.type},
                    {"value", p.type == 1 ? nlohmann::json(p.s) : nlohmann::json(p.f)}
                });
            }
            out["props"] = arr;
        }
    }

    void deserialize(const nlohmann::json& in) override {
        script_path = in.value("script_path", std::string());
        props.clear();
        auto arr = in.value("props", nlohmann::json::array());
        for (const auto& item : arr) {
            ScriptProp p;
            p.name = item.value("name", std::string());
            p.type = item.value("type", 0);
            if (p.type == 1) p.s = item.value("value", std::string());
            else p.f = item.value("value", 0.0f);
            props.push_back(std::move(p));
        }
    }

    // --- runtime state (not serialized; managed by ScriptSystem) ---
    bool script_loaded = false;
    bool start_called = false;
    bool reported_error = false;
    bool paused_on_error = false; // on_update/on_start 抛异常后暂停该实体，直到重载
    std::string last_error;
    std::vector<ScriptProp> props;

    // --- Godot-like Node scheduling ---
    int process_priority = 0; // 值越大 on_update 越先执行（默认 0）
    bool pause_mode = false;  // true = 全局暂停时仍执行 on_update（类比 process_mode）
};

} // namespace gryce_engine::components
