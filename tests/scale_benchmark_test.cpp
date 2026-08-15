#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "components/component_factory.h"
#include "components/mesh_renderer.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"

using namespace gryce_engine;

// 场景规模基准：500 / 2000 / 5000 实体的创建、组件查询与序列化耗时。
// 不断言精确阈值（机器差异大），只做数量级 sanity bound 并打印耗时，
// 供回归对比——若 Hierarchy/序列化/查询复杂度变化，耗时会出现明显跳变。
class ScaleBenchmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
    }

    static std::unique_ptr<scene::Scene> BuildScene(int n) {
        auto scene = std::make_unique<scene::Scene>("bench");
        for (int i = 0; i < n; ++i) {
            scene::Entity* e = scene->create_entity("Entity_" + std::to_string(i));
            e->add_component<components::MeshRenderer>("res:/models/cube.obj");
        }
        return scene;
    }

    template <typename F>
    static double Ms(F&& fn) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
};

TEST_F(ScaleBenchmarkTest, BuildQuerySerialize) {
    for (int n : {500, 2000, 5000}) {
        double build_ms = Ms([&] { BuildScene(n); });

        auto scene = BuildScene(n);
        double query_ms = Ms([&] {
            int count = 0;
            ecs::foreach_with_component<components::MeshRenderer>(
                *scene, [&](scene::Entity*, components::MeshRenderer*) { ++count; });
            EXPECT_EQ(count, n);
        });

        double serialize_ms = Ms([&] { scene::SceneSerializer::serialize(*scene); });

        // sanity bound：5000 实体序列化应远小于 5s（典型 ~50-300ms）
        EXPECT_LT(serialize_ms, 5000.0);
        EXPECT_LT(query_ms, 2000.0);

        std::cout << "[bench] n=" << n
                  << " build=" << build_ms << "ms"
                  << " query=" << query_ms << "ms"
                  << " serialize=" << serialize_ms << "ms\n";
    }
}
