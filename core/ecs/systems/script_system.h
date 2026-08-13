#pragma once

#include <vector>
#include <unordered_map>

#include "ecs/system.h"
#include "export.h"

namespace gryce_engine::components { class ScriptComponent; }
namespace gryce_engine::scene { class Entity; }

namespace gryce_engine::ecs {

/// GryceSRT driver: loads .lua scripts into per-component environments and
/// calls on_start / on_update(dt) / on_destroy, every invocation under pcall.
class GRYCE_API ScriptSystem : public ISystem {
public:
    const char* name() const override { return "ScriptSystem"; }
    Phase phase() const override { return Phase::Update; }
    int priority() const override { return -100; }

    void on_update(scene::Scene& scene, float dt) override;
    void on_shutdown(scene::Scene& scene) override;

    /// Unloads every loaded script; they reload on the next update.
    void reload_all();
    void sync_props_from_env(components::ScriptComponent* comp);
    bool get_prop(components::ScriptComponent* comp, const char* name,
                  int& out_type, float& out_f, std::string& out_s);
    bool set_prop(components::ScriptComponent* comp, const char* name, float value);
    bool set_prop(components::ScriptComponent* comp, const char* name, const std::string& value);

private:
    void write_prop_to_env(components::ScriptComponent* comp, const char* name, float value);
    void write_prop_to_env(components::ScriptComponent* comp, const char* name, const std::string& value);

    void process_entity(scene::Entity* e, float dt);
    bool load(components::ScriptComponent* comp);
    void unload(components::ScriptComponent* comp);
    void call_method(components::ScriptComponent* comp, const char* method,
                     float arg = 0.0f, bool has_arg = false);
    void handle_error(components::ScriptComponent* comp);

    std::vector<components::ScriptComponent*> loaded_;
    std::vector<components::ScriptComponent*> seen_;
    // Cached script source per res: path, so N entities sharing one script
    // read + compile from memory instead of touching the disk every frame.
    std::unordered_map<std::string, std::string> source_cache_;
};

} // namespace gryce_engine::ecs
