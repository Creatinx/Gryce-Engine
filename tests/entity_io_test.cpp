#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "GryceCore/core_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/types.h"

// 实体子树 JSON 导出/导入 C API 测试（Undo/Redo、剪贴板、Prefab 的共用基础）。
class EntityIoTest : public ::testing::Test {
protected:
    void SetUp() override {
        GCoreInitDesc desc{};
        desc.version = sizeof(GCoreInitDesc);
        desc.project_root = GRYCE_TEST_PROJECT_ROOT;
        desc.enable_reflection = true;
        ASSERT_EQ(GCore_Init(&desc), 0);
    }

    void TearDown() override {
        GCore_Shutdown();
    }

    static GEntityHandle CreateEntity(const char* name, GEntityHandle parent = 0) {
        GCommand cmd{};
        cmd.type = ECMD_CREATE_ENTITY;
        struct Payload { char name[128]; GEntityHandle parent; } p{};
        std::strncpy(p.name, name, sizeof(p.name) - 1);
        p.parent = parent;
        std::memcpy(cmd.payload, &p, sizeof(p));
        GCore_PushCommand(&cmd);
        GCore_BeginFrame(0.016f);
        GCore_EndFrame();

        // 新实体是句柄最大的一个（handle 单调递增）
        GEntityHandle best = 0;
        const int count = GEntity_GetCount();
        for (int i = 0; i < count; ++i) {
            GEntityHandle h = GEntity_GetAt(i);
            if (h > best) best = h;
        }
        return best;
    }
};

TEST_F(EntityIoTest, ExportImportRoundTrip) {
    GEntityHandle cube = CreateEntity("Cube");
    ASSERT_NE(cube, 0);

    GVec3 pos{1.0f, 2.0f, 3.0f};
    ASSERT_EQ(GEntity_SetLocalPosition(cube, &pos), 0);

    char json[8192] = {};
    int len = GEntity_ExportJson(cube, json, sizeof(json));
    ASSERT_GT(len, 0);
    EXPECT_NE(std::string(json).find("\"Cube\""), std::string::npos);

    const int before = GEntity_GetCount();
    GEntityHandle restored = GEntity_ImportJson(json, 0);
    ASSERT_NE(restored, 0);
    EXPECT_EQ(GEntity_GetCount(), before + 1);

    char name[128] = {};
    ASSERT_GT(GEntity_GetName(restored, name, sizeof(name)), 0);
    EXPECT_EQ(std::string(name), "Cube");

    GVec3 out{};
    ASSERT_EQ(GEntity_GetLocalPosition(restored, &out), 0);
    EXPECT_FLOAT_EQ(out.x, 1.0f);
    EXPECT_FLOAT_EQ(out.y, 2.0f);
    EXPECT_FLOAT_EQ(out.z, 3.0f);
}

TEST_F(EntityIoTest, ExportPreservesChildHierarchy) {
    GEntityHandle parent = CreateEntity("Parent");
    ASSERT_NE(parent, 0);
    GEntityHandle child = CreateEntity("Child", parent);
    ASSERT_NE(child, 0);

    char json[8192] = {};
    ASSERT_GT(GEntity_ExportJson(parent, json, sizeof(json)), 0);

    GEntityHandle restored = GEntity_ImportJson(json, 0);
    ASSERT_NE(restored, 0);
    EXPECT_EQ(GEntity_GetChildCount(restored), 1);

    GEntityHandle childHandle = GEntity_GetChildAt(restored, 0);
    char name[128] = {};
    ASSERT_GT(GEntity_GetName(childHandle, name, sizeof(name)), 0);
    EXPECT_EQ(std::string(name), "Child");
}

TEST_F(EntityIoTest, HierarchyParentChildTree) {
    GEntityHandle parent = CreateEntity("Parent");
    ASSERT_NE(parent, 0);
    GEntityHandle child = CreateEntity("Child", parent);
    ASSERT_NE(child, 0);

    // 根级实体的 parent 返回 0（合成根不暴露给编辑器）
    EXPECT_EQ(GEntity_GetParent(parent), GEntityHandle(0));
    // 子实体能正确回溯到父实体
    EXPECT_EQ(GEntity_GetParent(child), parent);
    // 父实体能枚举子节点（树状结构的数据基础）
    EXPECT_EQ(GEntity_GetChildCount(parent), 1);
    EXPECT_EQ(GEntity_GetChildAt(parent, 0), child);
    // 层级路径
    char path[256] = {};
    ASSERT_GT(GEntity_GetPath(child, path, sizeof(path)), 0);
    EXPECT_EQ(std::string(path), "Parent/Child");
}
