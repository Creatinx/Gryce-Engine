#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "components/component_factory.h"
#include "components/2d/basic_rect.h"
#include "ecs/world.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"

using namespace gryce_engine;

// Play Mode 快照/回滚机制测试：
// 模拟 core/api/core_api.cpp 中 ECMD_PLAY_MODE / ECMD_STOP_MODE 的
// serialize -> deserialize -> attach_scene 流程，验证 Stop 后场景
// 恢复到播放前状态（播放期间的改动被丢弃）。
class PlayModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
    }
};

TEST_F(PlayModeTest, SnapshotRestoreRollsBackChanges) {
    auto world = std::make_unique<ecs::World>();
    auto scene = std::make_unique<scene::Scene>("play");

    scene::Entity* cube = scene->create_entity("Cube");
    cube->transform()->position = math::Vector3f(1.0f, 2.0f, 3.0f);

    auto child_ptr = std::make_unique<scene::Entity>("Child");
    scene::Entity* child = cube->add_child(std::move(child_ptr));
    child->transform()->position = math::Vector3f(0.5f, 0.5f, 0.5f);

    world->attach_scene(std::move(scene));

    // 进入 Play：保存快照
    std::string snapshot = scene::SceneSerializer::serialize(*world->scene()).dump();
    ASSERT_FALSE(snapshot.empty());

    // 播放期间：改 Transform、改名、新增实体
    scene::Entity* live = world->scene()->find_entity_by_name("Cube");
    ASSERT_NE(live, nullptr);
    live->transform()->position = math::Vector3f(99.0f, 99.0f, 99.0f);
    live->set_name("Cube_modified");
    world->scene()->create_entity("PlaytimeSpawn");

    // 停止 Play：从快照恢复
    auto restored = scene::SceneSerializer::deserialize(nlohmann::json::parse(snapshot));
    ASSERT_NE(restored, nullptr);
    world->attach_scene(std::move(restored));

    // 断言：播放前的状态完整还原
    scene::Entity* cube2 = world->scene()->find_entity_by_name("Cube");
    ASSERT_NE(cube2, nullptr);
    EXPECT_FLOAT_EQ(cube2->transform()->position.x, 1.0f);
    EXPECT_FLOAT_EQ(cube2->transform()->position.y, 2.0f);
    EXPECT_FLOAT_EQ(cube2->transform()->position.z, 3.0f);
    EXPECT_EQ(world->scene()->find_entity_by_name("Cube_modified"), nullptr);
    EXPECT_EQ(world->scene()->find_entity_by_name("PlaytimeSpawn"), nullptr);
    EXPECT_EQ(world->scene()->find_entity_by_name("Child"), cube2->children()[0].get());
}

TEST_F(PlayModeTest, SnapshotRestorePreservesComponents) {
    auto world = std::make_unique<ecs::World>();
    auto scene = std::make_unique<scene::Scene>("play_comp");

    scene::Entity* e = scene->create_entity("Ball");
    e->add_component<components::d2::basic_rect::ColorRect>(10.0f, 20.0f, render::Color::red());

    world->attach_scene(std::move(scene));
    std::string snapshot = scene::SceneSerializer::serialize(*world->scene()).dump();

    // 播放期间移除组件
    scene::Entity* live = world->scene()->find_entity_by_name("Ball");
    ASSERT_NE(live, nullptr);
    live->remove_component(live->get_component<components::d2::basic_rect::ColorRect>());

    auto restored = scene::SceneSerializer::deserialize(nlohmann::json::parse(snapshot));
    world->attach_scene(std::move(restored));

    scene::Entity* ball = world->scene()->find_entity_by_name("Ball");
    ASSERT_NE(ball, nullptr);
    EXPECT_NE(ball->get_component<components::d2::basic_rect::ColorRect>(), nullptr);
}
