#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "script/runtime/script_vm.h"
#include "script/bindings/engine_bridge.h"
#include "script/bindings/math_bridge.h"
#include "script/runtime/script_context.h"

using namespace GryceEngineUtils::script;

// ============================================================================
// engine.* API 绑定测试
// ============================================================================

class JsEngineBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(vm.init());
        register_engine_bindings(vm.context());
        register_math_bindings(vm.context());
    }

    void TearDown() override {
        vm.shutdown();
    }

    ScriptVM vm;
};

TEST_F(JsEngineBridgeTest, EngineObjectExists) {
    auto result = vm.eval("typeof engine");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineLogInfo) {
    // 验证 engine.log.info 存在且不崩溃
    auto result = vm.eval("typeof engine.log.info");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineVersion) {
    auto result = vm.eval("engine.version()");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.result.find("GryceEngine") != std::string::npos);
}

TEST_F(JsEngineBridgeTest, EngineSelf) {
    // 设置当前实体
    ScriptContext::instance().set_current_entity(42);
    auto result = vm.eval("engine.self()");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "42");
}

TEST_F(JsEngineBridgeTest, EngineEntityAPI) {
    // 验证 entity 子对象存在
    auto result = vm.eval("typeof engine.entity");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineEntityFind) {
    // entity.find 应该存在且可调用
    auto result = vm.eval("typeof engine.entity.find");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntityCreate) {
    // entity.create 应该存在且可调用
    auto result = vm.eval("typeof engine.entity.create");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntityGetName) {
    auto result = vm.eval("typeof engine.entity.get_name");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntityGetTransform) {
    auto result = vm.eval("typeof engine.entity.get_transform");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntitySetTransform) {
    auto result = vm.eval("typeof engine.entity.set_transform");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineComponentAPI) {
    auto result = vm.eval("typeof engine.component");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineComponentHas) {
    auto result = vm.eval("typeof engine.component.has");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineComponentGet) {
    auto result = vm.eval("typeof engine.component.get");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineComponentSet) {
    auto result = vm.eval("typeof engine.component.set");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineStateAPI) {
    auto result = vm.eval("typeof engine.state");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineStateSet) {
    auto result = vm.eval("typeof engine.state.set");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineStateGet) {
    auto result = vm.eval("typeof engine.state.get");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineInputAPI) {
    auto result = vm.eval("typeof engine.input");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineInputKeyDown) {
    auto result = vm.eval("typeof engine.input.key_down");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineInputMousePos) {
    auto result = vm.eval("typeof engine.input.mouse_pos");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineTimeAPI) {
    auto result = vm.eval("typeof engine.time");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineTimeDelta) {
    // 设置 delta time
    ScriptContext::instance().set_delta_time(0.016f);
    auto result = vm.eval("engine.time.delta()");
    EXPECT_TRUE(result.success);
    // QuickJS 的 JS_ToCString 会输出完整浮点数精度
    // 0.016f 转换为 double 后约为 0.01600000075995922
    EXPECT_NEAR(std::stod(result.result), 0.016, 0.0001);
}

TEST_F(JsEngineBridgeTest, EngineTimeElapsed) {
    ScriptContext::instance().set_elapsed_time(10.5f);
    auto result = vm.eval("engine.time.elapsed()");
    EXPECT_TRUE(result.success);
    EXPECT_NEAR(std::stod(result.result), 10.5, 0.0001);
}

TEST_F(JsEngineBridgeTest, EngineSceneAPI) {
    auto result = vm.eval("typeof engine.scene");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineSceneLoad) {
    auto result = vm.eval("typeof engine.scene.load");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineAudioAPI) {
    auto result = vm.eval("typeof engine.audio");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineFxAPI) {
    auto result = vm.eval("typeof engine.fx");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EngineJsonAPI) {
    auto result = vm.eval("typeof engine.json");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EnginePhysicsAPI) {
    auto result = vm.eval("typeof engine.physics");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "object");
}

TEST_F(JsEngineBridgeTest, EnginePhysicsSetGravity) {
    auto result = vm.eval("typeof engine.physics.set_gravity");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineLoadSceneAlias) {
    // 旧 UI 兼容的 engine.loadScene
    auto result = vm.eval("typeof engine.loadScene");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineShowDialog) {
    auto result = vm.eval("typeof engine.showDialog");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineCloseDialog) {
    auto result = vm.eval("typeof engine.closeDialog");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineBind) {
    auto result = vm.eval("typeof engine.bind");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "function");
}

// ============================================================================
// engine.state 双向读写（跨 eval 调用保持）
// ============================================================================

TEST_F(JsEngineBridgeTest, EngineStateSetGetRoundTrip) {
    EXPECT_TRUE(vm.eval("engine.state.set('hp', '100')").success);
    auto r = vm.eval("engine.state.get('hp')");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "100");
}

TEST_F(JsEngineBridgeTest, EngineStateHas) {
    EXPECT_TRUE(vm.eval("engine.state.set('score', '42')").success);
    EXPECT_EQ(vm.eval("engine.state.has('score')").result, "true");
    EXPECT_EQ(vm.eval("engine.state.has('missing')").result, "false");
}

TEST_F(JsEngineBridgeTest, EngineStateNumericRoundTrip) {
    EXPECT_TRUE(vm.eval("engine.state.set('gold', 250)").success);
    EXPECT_EQ(vm.eval("engine.state.get('gold')").result, "250");
}

TEST_F(JsEngineBridgeTest, EngineStatePersistsAcrossEvals) {
    // 第一次调用写入，第二次调用读取——验证跨调用状态保持
    EXPECT_TRUE(vm.eval("engine.state.set('level', 3)").success);
    auto r = vm.eval("(function(){ return engine.state.get('level'); })()");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "3");
}

// ============================================================================
// engine.* 顶层 UI API 可调用性
// ============================================================================

TEST_F(JsEngineBridgeTest, EngineLoadSceneCallable) {
    // 无 UI 场景时调用不应崩溃；场景引用为空时安全返回
    auto r = vm.eval("engine.loadScene('nonexistent')");
    EXPECT_TRUE(r.success);
}

TEST_F(JsEngineBridgeTest, EnginePlaySoundCallable) {
    auto r = vm.eval("typeof engine.playSound");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineGetStringCallable) {
    auto r = vm.eval("typeof engine.getString");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineLogErrorExists) {
    auto r = vm.eval("typeof engine.log.error");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineComponentSetGetWithEntity) {
    // 设置当前实体后 component.get 可调用（无组件时安全返回）
    ScriptContext::instance().set_current_entity(7);
    auto r = vm.eval("engine.component.get('Transform')");
    EXPECT_TRUE(r.success);
}

TEST_F(JsEngineBridgeTest, EngineEntityFindAllExists) {
    auto r = vm.eval("typeof engine.entity.find_all");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntityDestroyExists) {
    auto r = vm.eval("typeof engine.entity.destroy");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineEntityAabbExists) {
    auto r = vm.eval("typeof engine.entity.aabb");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineJsonReadExists) {
    auto r = vm.eval("typeof engine.json.read");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineFxBurstExists) {
    auto r = vm.eval("typeof engine.fx.burst");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineAudioPlayOnExists) {
    auto r = vm.eval("typeof engine.audio.play_on");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EnginePhysicsGetGravityExists) {
    auto r = vm.eval("typeof engine.physics.get_gravity");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineInputMouseDownExists) {
    auto r = vm.eval("typeof engine.input.mouse_down");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineSceneCurrentExists) {
    auto r = vm.eval("typeof engine.scene.current");
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.result, "function");
}

TEST_F(JsEngineBridgeTest, EngineLogInfoCallable) {
    // 直接调用不崩溃
    auto r = vm.eval("engine.log.info('hello from test')");
    EXPECT_TRUE(r.success);
}

TEST_F(JsEngineBridgeTest, EngineTimeDeltaCallable) {
    ScriptContext::instance().set_delta_time(0.016f);
    auto r = vm.eval("engine.time.delta()");
    EXPECT_TRUE(r.success);
    EXPECT_NEAR(std::stod(r.result), 0.016, 0.0001);
}

// ============================================================================
// ScriptContext 测试
// ============================================================================

TEST(ScriptContextTest, Singleton) {
    auto& ctx1 = ScriptContext::instance();
    auto& ctx2 = ScriptContext::instance();
    EXPECT_EQ(&ctx1, &ctx2);
}

TEST(ScriptContextTest, CurrentEntity) {
    auto& ctx = ScriptContext::instance();
    ctx.set_current_entity(123);
    EXPECT_EQ(ctx.current_entity(), 123);
}

TEST(ScriptContextTest, State) {
    auto& ctx = ScriptContext::instance();
    ctx.set_state("score", "100");
    EXPECT_TRUE(ctx.has_state("score"));
    EXPECT_EQ(ctx.get_state("score"), "100");
    EXPECT_FALSE(ctx.has_state("nonexistent"));
}

TEST(ScriptContextTest, KeyState) {
    auto& ctx = ScriptContext::instance();
    ctx.set_key_state(32, true); // space key
    EXPECT_TRUE(ctx.is_key_down(32));
    EXPECT_FALSE(ctx.is_key_down(33));
}

TEST(ScriptContextTest, MouseState) {
    auto& ctx = ScriptContext::instance();
    ctx.set_mouse_pos(640.0f, 480.0f);
    float x, y;
    ctx.get_mouse_pos(&x, &y);
    EXPECT_FLOAT_EQ(x, 640.0f);
    EXPECT_FLOAT_EQ(y, 480.0f);

    ctx.set_mouse_button(0, true);
    EXPECT_TRUE(ctx.is_mouse_down(0));
}

TEST(ScriptContextTest, Time) {
    auto& ctx = ScriptContext::instance();
    ctx.set_delta_time(0.016f);
    ctx.set_elapsed_time(60.0f);
    EXPECT_FLOAT_EQ(ctx.delta_time(), 0.016f);
    EXPECT_FLOAT_EQ(ctx.elapsed_time(), 60.0f);
}