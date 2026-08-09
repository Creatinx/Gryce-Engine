#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "GryceCore/core_api.h"
#include "GryceCore/component_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/types.h"

// 2D 组件反射桥回归测试：
// 2D 组件位于 d2::basic_rect 等嵌套命名空间，reflection_lookup_name 必须
// 能映射到注册的短名，Inspector 才能枚举/编辑其字段。
class Reflection2DTest : public ::testing::Test {
protected:
    std::string root_;

    void SetUp() override {
        root_ = std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures";
        GCoreInitDesc desc{};
        desc.version = sizeof(GCoreInitDesc);
        desc.project_root = root_.c_str();
        desc.enable_reflection = true;
        ASSERT_EQ(GCore_Init(&desc), 0);
    }

    void TearDown() override {
        GCore_Shutdown();
    }

    static GEntityHandle CreateEntity(const char* name)
    {
        GCommand cmd{};
        cmd.type = ECMD_CREATE_ENTITY;
        struct Payload { char name[128]; GEntityHandle parent; } p{};
        std::strncpy(p.name, name, sizeof(p.name) - 1);
        std::memcpy(cmd.payload, &p, sizeof(p));
        GCore_PushCommand(&cmd);
        GCore_BeginFrame(0.016f);
        GCore_EndFrame();

        GEntityHandle best = 0;
        const int count = GEntity_GetCount();
        for (int i = 0; i < count; ++i)
        {
            GEntityHandle h = GEntity_GetAt(i);
            if (h > best) best = h;
        }
        return best;
    }
};

TEST_F(Reflection2DTest, ColorRectFieldsResolveAndEditable)
{
    GEntityHandle e = CreateEntity("Rect");
    ASSERT_NE(e, 0);

    // 添加 ColorRect（注册表短名哈希）
    ASSERT_EQ(GComponent_AddComponent(e, std::hash<std::string>{}("ColorRect")), 0);
    GCore_BeginFrame(0.016f);
    GCore_EndFrame();

    // 组件属性操作使用 typeid 全名哈希
    uint64_t hash = 0;
    ASSERT_EQ(GComponent_GetTypeHashAt(e, 0, &hash), 0);

    // 反射字段必须能解析（嵌套命名空间映射 bug 的回归点）
    const int count = GComponent_GetPropertyCount(e, hash);
    ASSERT_GT(count, 0);

    bool found_width = false;
    for (int i = 0; i < count; ++i)
    {
        char name[128] = {};
        int type = 0, size = 0;
        if (GComponent_GetPropertyInfo(e, hash, i, name, sizeof(name), &type, &size) == 0 &&
            std::string(name) == "width")
        {
            found_width = true;
            break;
        }
    }
    EXPECT_TRUE(found_width);

    // 读写字段
    float w = 200.0f;
    ASSERT_EQ(GComponent_SetProperty(e, hash, "width", &w, sizeof(w)), 0);
    float out = 0.0f;
    ASSERT_EQ(GComponent_GetProperty(e, hash, "width", &out, sizeof(out)), 0);
    EXPECT_FLOAT_EQ(out, 200.0f);
}
