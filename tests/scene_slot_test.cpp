#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

#include "GryceCore/core_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/types.h"

// 2D/3D 双场景槽测试：切换编辑器模式时场景内存保留（热切换），
// ReleaseMode 释放指定槽。
class SceneSlotTest : public ::testing::Test {
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
        // 清理测试期间创建的缓冲场景文件，避免污染 fixtures
        std::filesystem::remove(root_ + "/scenes/scene_2d.gesc");
        std::filesystem::remove(root_ + "/scenes/scene_3d.gesc");
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

    static bool EntityExists(const char* name)
    {
        const int count = GEntity_GetCount();
        for (int i = 0; i < count; ++i)
        {
            GEntityHandle h = GEntity_GetAt(i);
            if (h == 0) continue;
            char buf[128] = {};
            if (GEntity_GetName(h, buf, sizeof(buf)) > 0 && std::strcmp(buf, name) == 0) return true;
        }
        return false;
    }
};

TEST_F(SceneSlotTest, HotSwapKeepsBothScenesInMemory)
{
    // 默认 2D 模式 → 切到 3D 建实体
    ASSERT_EQ(GScene_GetMode(), 0);
    ASSERT_EQ(GScene_SetMode(1), 0);
    ASSERT_EQ(GScene_GetMode(), 1);
    ASSERT_NE(CreateEntity("Cube3D"), 0);
    EXPECT_TRUE(EntityExists("Cube3D"));

    // 切到 2D：3D 场景存入槽内，2D 槽为空 → 空场景
    ASSERT_EQ(GScene_SetMode(0), 0);
    EXPECT_FALSE(EntityExists("Cube3D"));
    EXPECT_EQ(GEntity_GetCount(), 0);

    // 2D 槽建实体
    ASSERT_NE(CreateEntity("Rect2D"), 0);
    EXPECT_TRUE(EntityExists("Rect2D"));

    // 切回 3D：内存保留，Cube3D 还在
    ASSERT_EQ(GScene_SetMode(1), 0);
    EXPECT_TRUE(EntityExists("Cube3D"));
    EXPECT_FALSE(EntityExists("Rect2D"));

    // 切回 2D：Rect2D 还在
    ASSERT_EQ(GScene_SetMode(0), 0);
    EXPECT_TRUE(EntityExists("Rect2D"));
    EXPECT_FALSE(EntityExists("Cube3D"));
}

TEST_F(SceneSlotTest, ReleaseModeFreesInactiveSlot)
{
    ASSERT_EQ(GScene_SetMode(1), 0);
    ASSERT_NE(CreateEntity("Cube3D"), 0);

    ASSERT_EQ(GScene_SetMode(0), 0);
    EXPECT_TRUE(GScene_HasScene(1));

    // 释放 3D 槽（当前在 2D 模式）
    ASSERT_EQ(GScene_ReleaseMode(1), 0);
    EXPECT_FALSE(GScene_HasScene(1));

    // 切回 3D：槽为空 → 新建空场景
    ASSERT_EQ(GScene_SetMode(1), 0);
    EXPECT_FALSE(EntityExists("Cube3D"));
    EXPECT_EQ(GEntity_GetCount(), 0);
}
