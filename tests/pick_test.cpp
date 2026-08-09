#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "GryceCore/core_api.h"
#include "GryceCore/component_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/types.h"

// 射线拾取 C API 测试：带 MeshRenderer 的实体应能被世界空间射线命中。
class PickTest : public ::testing::Test {
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

    static GEntityHandle CreateCube()
    {
        GCommand cmd{};
        cmd.type = ECMD_CREATE_ENTITY;
        struct Payload { char name[128]; GEntityHandle parent; } p{};
        std::strncpy(p.name, "PickCube", sizeof(p.name) - 1);
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

TEST_F(PickTest, RayHitsMeshEntity)
{
    GEntityHandle cube = CreateCube();
    ASSERT_NE(cube, 0);

    // 添加 MeshRenderer 并设置网格路径（reflection 字段名 mesh_path，字符串）
    ASSERT_EQ(GComponent_AddComponent(cube, std::hash<std::string>{}("MeshRenderer")), 0);
    GCore_BeginFrame(0.016f);
    GCore_EndFrame();

    // 属性操作使用 typeid 全名的哈希（与编辑器 Inspector 一致）
    uint64_t hash = 0;
    ASSERT_EQ(GComponent_GetTypeHashAt(cube, 0, &hash), 0);

    const std::string path = "res:/models/cube.obj";
    {
        // GComponent_SetProperty 需要 256 字节字符串缓冲（反射字符串字段上限）
        char buf[256] = {};
        std::memcpy(buf, path.c_str(), path.size() + 1);
        ASSERT_EQ(GComponent_SetProperty(cube, hash, "mesh_path", buf, sizeof(buf)), 0);
    }

    // 立方体在原点（默认 transform）；从 (0,0,5) 向 -Z 发射射线
    GVec3 origin{0.0f, 0.0f, 5.0f};
    GVec3 dir{0.0f, 0.0f, -1.0f};
    GEntityHandle hit = GScene_PickRay(&origin, &dir, 100.0f);
    EXPECT_EQ(hit, cube);

    // 完全偏离立方体的射线不应命中
    GVec3 missOrigin{5.0f, 5.0f, 5.0f};
    GVec3 missDir{0.0f, 0.0f, -1.0f};
    EXPECT_EQ(GScene_PickRay(&missOrigin, &missDir, 100.0f), GEntityHandle(0));
}
