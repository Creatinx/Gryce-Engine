#include <gtest/gtest.h>

#include <string>

#include "components/component_factory.h"
#include "components/script_component.h"
#include "ecs/systems/script_system.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "runtime/engine_context.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "script/runtime/script_context.h"

using namespace gryce_engine;

// ============================================================================
// ScriptSystem：QuickJS 驱动的 ECS 脚本生命周期
// ============================================================================

class ScriptSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
        resources::Project::instance().set_root(
            std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures");
        GryceEngineUtils::script::ScriptContext::instance().set_current_entity(0);
    }

    scene::Entity* add_script_entity(scene::Scene& scene, const char* script) {
        scene::Entity* e = scene.create_entity("Scripted");
        auto* comp = e->add_component<components::ScriptComponent>();
        comp->script_path = std::string("res:/") + script;
        return e;
    }
};

// ============================================================================
// 加载与生命周期
// ============================================================================

TEST_F(ScriptSystemTest, LoadsScriptOnFirstUpdate) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/player.js");
    auto* comp = e->get_component<components::ScriptComponent>();
    ASSERT_NE(comp, nullptr);

    system.on_update(scene, 0.016f);

    EXPECT_TRUE(comp->script_loaded);
    EXPECT_FALSE(comp->paused_on_error);
}

TEST_F(ScriptSystemTest, MissingOptionalMethodDoesNotPause) {
    // minimal.js 没有 on_start / on_destroy，只有 on_update；缺失方法应静默跳过
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/minimal.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);

    EXPECT_TRUE(comp->script_loaded);
    EXPECT_FALSE(comp->paused_on_error);
    EXPECT_TRUE(comp->last_error.empty());
}

// ============================================================================
// props 双向同步
// ============================================================================

TEST_F(ScriptSystemTest, PropsSyncFromEnv) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/player.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);

    int type = -1;
    float f = 0.0f;
    std::string s;
    ASSERT_TRUE(system.get_prop(comp, "hp", type, f, s));
    EXPECT_EQ(type, 0); // float
    EXPECT_FLOAT_EQ(f, 100.0f);

    ASSERT_TRUE(system.get_prop(comp, "name", type, f, s));
    EXPECT_EQ(type, 1); // string
    EXPECT_EQ(s, "player");
}

TEST_F(ScriptSystemTest, PropsWriteBackToJs) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/player.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);
    ASSERT_TRUE(comp->script_loaded);

    // C++ 侧修改 prop，写回 JS props 对象
    ASSERT_TRUE(system.set_prop(comp, "hp", 250.0f));

    // 下一帧脚本 on_update 读到 250 并自增到 251
    system.on_update(scene, 0.016f);
    system.sync_props_from_env(comp);

    int type = -1;
    float f = 0.0f;
    std::string s;
    ASSERT_TRUE(system.get_prop(comp, "hp", type, f, s));
    EXPECT_FLOAT_EQ(f, 251.0f);
}

TEST_F(ScriptSystemTest, PropsWriteBackString) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/player.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);

    ASSERT_TRUE(system.set_prop(comp, "name", std::string("hero")));
    system.sync_props_from_env(comp);

    int type = -1;
    float f = 0.0f;
    std::string s;
    ASSERT_TRUE(system.get_prop(comp, "name", type, f, s));
    EXPECT_EQ(s, "hero");
}

// ============================================================================
// 热重载保留 props
// ============================================================================

TEST_F(ScriptSystemTest, HotReloadPreservesProps) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/player.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);
    ASSERT_TRUE(comp->script_loaded);

    // 修改 prop 到 300（模拟 Inspector 修改）
    ASSERT_TRUE(system.set_prop(comp, "hp", 300.0f));

    // 热重载：卸载全部 -> 下次 on_update 重新加载
    system.reload_all();
    system.on_update(scene, 0.016f);
    ASSERT_TRUE(comp->script_loaded);

    // 重载后 prop 应恢复为 300 并写回新模块；
    // 同帧 on_update 会自增到 301，恰好证明 300 的修改被保留
    system.sync_props_from_env(comp);
    int type = -1;
    float f = 0.0f;
    std::string s;
    ASSERT_TRUE(system.get_prop(comp, "hp", type, f, s));
    EXPECT_FLOAT_EQ(f, 301.0f);
}

// ============================================================================
// 错误暂停（PAUSED_ERROR）
// ============================================================================

TEST_F(ScriptSystemTest, UpdateErrorPausesEntity) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/error_script.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    // 第一次 update：on_update 抛异常
    system.on_update(scene, 0.016f);
    EXPECT_TRUE(comp->script_loaded);
    EXPECT_TRUE(comp->paused_on_error);
    EXPECT_FALSE(comp->last_error.empty());

    // 再次 update：实体已暂停，不应崩溃
    system.on_update(scene, 0.016f);
    EXPECT_TRUE(comp->paused_on_error);
}

TEST_F(ScriptSystemTest, ReloadClearsErrorPause) {
    ecs::ScriptSystem system;
    scene::Scene scene("test");
    auto* e = add_script_entity(scene, "scripts/error_script.js");
    auto* comp = e->get_component<components::ScriptComponent>();

    system.on_update(scene, 0.016f);
    EXPECT_TRUE(comp->paused_on_error);

    // unload/reload 清除错误暂停标志（脚本卸载后不再保持暂停态）
    system.reload_all();
    EXPECT_FALSE(comp->paused_on_error);
    EXPECT_FALSE(comp->script_loaded);
}
