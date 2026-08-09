#pragma once

#include <string>

#include "export.h"
#include "components/component.h"

namespace gryce_engine::components {

/// ScriptComponent (GryceSRT): binds a .lua file to an entity. The Lua
/// callbacks (on_start / on_update / on_destroy) are driven by ScriptSystem.
/// Only script_path + enabled are serialized; the Lua env/chunk references are
/// runtime state managed by ScriptSystem.
class GRYCE_API ScriptComponent : public Component {
public:
    std::string script_path;

    ScriptComponent() = default;

    const char* type() const override { return "Script"; }

    void serialize(nlohmann::json& out) const override {
        out["script_path"] = script_path;
    }

    void deserialize(const nlohmann::json& in) override {
        script_path = in.value("script_path", std::string());
    }

    // --- runtime state (not serialized; managed by ScriptSystem) ---
    int env_ref = -1;       // lua registry ref to the per-component env table
    int chunk_ref = -1;     // lua registry ref to the loaded chunk
    bool script_loaded = false;
    bool start_called = false;
    bool reported_error = false;
    std::string last_error;
};

} // namespace gryce_engine::components
