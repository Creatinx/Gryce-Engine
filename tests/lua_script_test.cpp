#include <gtest/gtest.h>

#include <string>

#include "script/lua_runtime.h"

using namespace gryce_engine;

// GryceSRT Lua 运行时冒烟测试：
// 直接驱动 script::LuaRuntime（GCore_Init 之外的最小路径），覆盖
// engine.state / engine.time / engine.input 绑定与错误处理。
// 注意：LuaRuntime 是全局单例，本测试独占 init/shutdown。
class LuaScriptTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& rt = script::LuaRuntime::instance();
        ASSERT_TRUE(rt.init()) << "Lua runtime failed to initialize";
    }

    void TearDown() override {
        script::LuaRuntime::instance().shutdown();
    }
};

TEST_F(LuaScriptTest, StateTablePersistsAcrossChunks) {
    auto& rt = script::LuaRuntime::instance();
    std::string err;

    EXPECT_TRUE(rt.run_string("engine.state.answer = 42", &err)) << err;
    EXPECT_TRUE(rt.run_string(
        "if engine.state.answer ~= 42 then error('state table lost') end", &err)) << err;
}

TEST_F(LuaScriptTest, TimeAndInputBindingsAreCallable) {
    auto& rt = script::LuaRuntime::instance();
    std::string err;

    // engine.time.delta() 默认 0，可调用即可（验证注册表完整）
    EXPECT_TRUE(rt.run_string("local d = engine.time.delta()", &err)) << err;
    // engine.input.* 读取运行时输入上下文；未注入事件时返回 false，不应崩溃
    EXPECT_TRUE(rt.run_string("local k = engine.input.key_down(32)", &err)) << err;
    EXPECT_TRUE(rt.run_string("local x, y = engine.input.mouse_pos()", &err)) << err;
    EXPECT_TRUE(rt.run_string("local locked = engine.input.mouse_locked()", &err)) << err;
}

TEST_F(LuaScriptTest, SyntaxErrorIsReported) {
    auto& rt = script::LuaRuntime::instance();
    std::string err;

    EXPECT_FALSE(rt.run_string("this is not valid lua", &err));
    EXPECT_FALSE(err.empty()) << "error message should be populated";
}

TEST_F(LuaScriptTest, RuntimeErrorIsReported) {
    auto& rt = script::LuaRuntime::instance();
    std::string err;

    EXPECT_FALSE(rt.run_string("error('boom')", &err));
    EXPECT_NE(err.find("boom"), std::string::npos) << "error should surface message, got: " << err;
}
