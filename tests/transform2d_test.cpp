#include <gtest/gtest.h>

#include <memory>

#include "components/2d/component_2d.h"
#include "components/2d/light_2d.h"
#include "components/node2d.h"
#include "components/transform.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

namespace {

constexpr float k_eps = 1e-3f;

// 在 parent 下创建一个仅含 2D 变换的子实体
scene::Entity* add_child_2d(scene::Entity* parent, const char* name,
                            float x, float y, float rot_z = 0.0f, float s = 1.0f) {
    auto e = std::make_unique<scene::Entity>(name);
    e->transform()->position = math::Vector3f(x, y, 0.0f);
    e->transform()->rotation = math::Quaternionf::from_axis_angle(
        math::Vector3f(0.0f, 0.0f, 1.0f), rot_z);
    e->transform()->scale = math::Vector3f(s, s, 1.0f);
    return parent->add_child(std::move(e));
}

} // namespace

// 顶层实体（父级是场景合成根节点，恒等变换）：世界 == 本地
TEST(Transform2DTest, RootLevelWorldEqualsLocal) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("A");
    e->transform()->position = math::Vector3f(10.0f, 20.0f, 0.0f);

    auto wt = components::d2::world_transform_2d(e);
    EXPECT_NEAR(wt.position.x, 10.0f, k_eps);
    EXPECT_NEAR(wt.position.y, 20.0f, k_eps);
    EXPECT_NEAR(wt.rotation, 0.0f, k_eps);
    EXPECT_NEAR(wt.scale.x, 1.0f, k_eps);
}

// 父链组合：平移 + 旋转 + 缩放逐层复合
TEST(Transform2DTest, ParentChainComposes) {
    scene::Scene scene("test");
    scene::Entity* parent = scene.create_entity("P");
    parent->transform()->position = math::Vector3f(100.0f, 50.0f, 0.0f);
    parent->transform()->rotation = math::Quaternionf::from_axis_angle(
        math::Vector3f(0.0f, 0.0f, 1.0f), math::to_radians(90.0f));
    parent->transform()->scale = math::Vector3f(2.0f, 2.0f, 1.0f);

    scene::Entity* child = add_child_2d(parent, "C", 10.0f, 0.0f);

    // 世界位置 = (100,50) + rot90( (2,2) * (10,0) ) = (100, 70)
    auto wt = components::d2::world_transform_2d(child);
    EXPECT_NEAR(wt.position.x, 100.0f, k_eps);
    EXPECT_NEAR(wt.position.y, 70.0f, k_eps);
    EXPECT_NEAR(wt.rotation, math::to_radians(90.0f), k_eps);
    EXPECT_NEAR(wt.scale.x, 2.0f, k_eps);
    EXPECT_NEAR(wt.scale.y, 2.0f, k_eps);

    // Component2D 的 position()/rotation()/scale() 使用同一世界语义
    auto* light = child->add_component<components::d2::light::Light2D>();
    EXPECT_NEAR(light->position().x, 100.0f, k_eps);
    EXPECT_NEAR(light->position().y, 70.0f, k_eps);
    EXPECT_NEAR(light->rotation(), math::to_radians(90.0f), k_eps);
    EXPECT_NEAR(light->scale().x, 2.0f, k_eps);
}

// top_level 节点忽略整条父链：世界 == 本地
TEST(Transform2DTest, TopLevelIgnoresParentChain) {
    scene::Scene scene("test");
    scene::Entity* parent = scene.create_entity("P");
    parent->transform()->position = math::Vector3f(100.0f, 50.0f, 0.0f);
    parent->transform()->rotation = math::Quaternionf::from_axis_angle(
        math::Vector3f(0.0f, 0.0f, 1.0f), math::to_radians(90.0f));
    parent->transform()->scale = math::Vector3f(2.0f, 2.0f, 1.0f);

    scene::Entity* child = add_child_2d(parent, "C", 10.0f, 5.0f);
    auto* n2d = child->add_component<components::Node2D>();
    n2d->top_level = true;

    auto wt = components::d2::world_transform_2d(child);
    EXPECT_NEAR(wt.position.x, 10.0f, k_eps);
    EXPECT_NEAR(wt.position.y, 5.0f, k_eps);
    EXPECT_NEAR(wt.rotation, 0.0f, k_eps);
    EXPECT_NEAR(wt.scale.x, 1.0f, k_eps);
}

// top_level 祖先：其自身忽略父链，其子级只组合到该祖先为止
TEST(Transform2DTest, TopLevelAncestorStopsChainBelowIt) {
    scene::Scene scene("test");
    scene::Entity* grandparent = scene.create_entity("G");
    grandparent->transform()->position = math::Vector3f(1000.0f, 1000.0f, 0.0f);

    scene::Entity* parent = add_child_2d(grandparent, "P", 100.0f, 50.0f);
    auto* n2d = parent->add_component<components::Node2D>();
    n2d->top_level = true;

    scene::Entity* child = add_child_2d(parent, "C", 10.0f, 0.0f);

    // parent 忽略 grandparent 的 (1000,1000)，child 世界 = parent.local + child.local
    auto wt = components::d2::world_transform_2d(child);
    EXPECT_NEAR(wt.position.x, 110.0f, k_eps);
    EXPECT_NEAR(wt.position.y, 50.0f, k_eps);
}
