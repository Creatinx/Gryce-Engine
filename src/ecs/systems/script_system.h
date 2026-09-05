#pragma once

#include <unordered_map>
#include <vector>

#include <quickjs/quickjs.h>

#include "ecs/system.h"
#include "export.h"
#include "script/runtime/script_vm.h"

namespace gryce_engine::components { class ScriptComponent; }
namespace gryce_engine::scene { class Entity; }
namespace gryce_engine::scene { class Scene; }

namespace gryce_engine::ecs {

/// GryceSRT driver: loads .js scripts into per-component module scopes and
/// calls on_start / on_update(dt) / on_destroy via QuickJS ScriptVM.
class GRYCE_API ScriptSystem : public ISystem {
public:
    const char* name() const override { return "ScriptSystem"; }
    Phase phase() const override { return Phase::Update; }
    int priority() const override { return -100; }

    void on_update(scene::Scene& scene, float dt) override;
    void on_shutdown(scene::Scene& scene) override;

    /// 析构时卸载全部脚本并释放模块命名空间，避免 QuickJS gc 对象泄漏。
    ~ScriptSystem() override { reload_all(); }

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

    void process_entity(components::ScriptComponent* comp, float dt);
    bool load(components::ScriptComponent* comp);
    void unload(components::ScriptComponent* comp);
    void call_method(components::ScriptComponent* comp, const char* method,
                     float arg = 0.0f, bool has_arg = false);
    void call_method_int(components::ScriptComponent* comp, const char* method,
                         int type, int a, int b, int c);
    void dispatch_input_events();
    void handle_error(components::ScriptComponent* comp);

    /// 全局 ScriptVM 实例（延迟初始化）
    static GryceEngineUtils::script::ScriptVM& vm();

    /// 组件 -> 模块命名空间映射
    std::unordered_map<components::ScriptComponent*, JSValue> module_ns_map_;

    std::vector<components::ScriptComponent*> loaded_;
    std::vector<components::ScriptComponent*> seen_;
    // Cached script source per script path
    std::unordered_map<std::string, std::string> source_cache_;
};

} // namespace gryce_engine::ecs