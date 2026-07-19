#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "animation/pose.h"
#include "components/skinned_mesh_renderer.h"
#include "ecs/systems/animator_system.h"
#include "render/skinned_vertex.h"
#include "scene/scene.h"
#include "scene/entity.h"

using namespace gryce_engine;

namespace {

// 构造最小蒙皮网格：3 顶点 + 2 条骨骼影响
assets::SkinnedMeshData make_test_skinned_mesh(bool with_influences = true) {
    assets::SkinnedMeshData data;
    data.vertices.resize(3);
    data.indices = {0, 1, 2};
    if (with_influences) {
        data.bone_influences.resize(3);
        data.bone_influences[0].add(0, 1.0f);
        data.bone_influences[1].add(1, 1.0f);
        data.bone_influences[2].add(0, 0.5f);
        data.bone_influences[2].add(1, 0.5f);
    }
    return data;
}

// 2 骨骼 skeleton（bind 全单位），bone1 有 1s 平移动画 (0,0,0)->(0,2,0)
std::shared_ptr<assets::SkinnedModelData> make_test_model() {
    auto model = std::make_shared<assets::SkinnedModelData>();
    model->has_skin = true;
    model->meshes.push_back(make_test_skinned_mesh());

    animation::Bone root;
    root.name = "root";
    root.parent_index = -1;
    root.inverse_bind_matrix = math::Matrix4f::identity();
    animation::Bone child;
    child.name = "child";
    child.parent_index = 0;
    child.inverse_bind_matrix = math::Matrix4f::identity();
    model->skeleton.bones = {root, child};

    animation::AnimationClip clip;
    clip.name = "move";
    clip.duration = 1.0f;
    animation::BoneTrack track;
    track.bone_index = 1;
    track.position_keys = {{0.0f, math::Vector3f(0.0f, 0.0f, 0.0f)},
                           {1.0f, math::Vector3f(0.0f, 2.0f, 0.0f)}};
    clip.tracks.push_back(track);
    model->animations.push_back(clip);
    return model;
}

} // namespace

// ---------------------------------------------------------------------------
// 顶点打包
// ---------------------------------------------------------------------------

TEST(SkinnedVertexTest, PacksBoneInfluences) {
    auto data = make_test_skinned_mesh();
    auto verts = render::build_skinned_vertices(data);

    ASSERT_EQ(verts.size(), 3u);
    // 顶点 0：bone0 权重 1.0
    EXPECT_EQ(verts[0].bone_ids[0], 0u);
    EXPECT_FLOAT_EQ(verts[0].weights[0], 1.0f);
    EXPECT_FLOAT_EQ(verts[0].weights[1], 0.0f);
    // 顶点 2：bone0/bone1 各 0.5
    EXPECT_EQ(verts[2].bone_ids[0], 0u);
    EXPECT_EQ(verts[2].bone_ids[1], 1u);
    EXPECT_FLOAT_EQ(verts[2].weights[0], 0.5f);
    EXPECT_FLOAT_EQ(verts[2].weights[1], 0.5f);
    // 空槽清零
    EXPECT_EQ(verts[0].bone_ids[3], 0u);
    EXPECT_FLOAT_EQ(verts[1].weights[2], 0.0f);
}

TEST(SkinnedVertexTest, NoSkinFallsBackToZeroWeights) {
    auto data = make_test_skinned_mesh(false);
    auto verts = render::build_skinned_vertices(data);

    ASSERT_EQ(verts.size(), 3u);
    for (const auto& v : verts) {
        for (int k = 0; k < 4; ++k) {
            EXPECT_EQ(v.bone_ids[k], 0u);
            EXPECT_FLOAT_EQ(v.weights[k], 0.0f);
        }
    }
}

TEST(SkinnedVertexTest, MismatchedInfluenceCountFallsBack) {
    auto data = make_test_skinned_mesh(false);
    data.bone_influences.resize(1); // 与 vertices(3) 数量不符
    data.bone_influences[0].add(1, 1.0f);
    auto verts = render::build_skinned_vertices(data);

    ASSERT_EQ(verts.size(), 3u);
    for (const auto& v : verts) {
        EXPECT_FLOAT_EQ(v.weights[0], 0.0f);
    }
}

TEST(SkinnedVertexLayoutTest, LayoutMatchesShaderContract) {
    EXPECT_EQ(sizeof(render::SkinnedVertexGPU), 88u);

    auto layout = render::skinned_vertex_layout();
    EXPECT_EQ(layout.stride, 88u);
    ASSERT_EQ(layout.attributes.size(), 7u);

    // location 5 = bone ids（UInt4，整数属性，offset 56）
    EXPECT_EQ(layout.attributes[5].location, 5u);
    EXPECT_EQ(layout.attributes[5].type, render::VertexType::UInt4);
    EXPECT_EQ(layout.attributes[5].offset, 56u);
    EXPECT_TRUE(render::is_integer_vertex_type(layout.attributes[5].type));
    // location 6 = weights（Float4，offset 72）
    EXPECT_EQ(layout.attributes[6].location, 6u);
    EXPECT_EQ(layout.attributes[6].type, render::VertexType::Float4);
    EXPECT_EQ(layout.attributes[6].offset, 72u);
    EXPECT_FALSE(render::is_integer_vertex_type(layout.attributes[6].type));
    // 前 5 个属性与普通 MeshVertex 布局一致（GL 普通/蒙皮 mesh 可共享 shader 输入语义）
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(layout.attributes[i].location, i);
    }
}

// ---------------------------------------------------------------------------
// AnimatorSystem
// ---------------------------------------------------------------------------

TEST(AnimatorSystemTest, AdvancesTimeAndSetsPalette) {
    scene::Scene scene("anim_test");
    scene::Entity* e = scene.create_entity("actor");
    auto* mr = e->add_component<components::SkinnedMeshRenderer>("test_model");
    mr->set_model(make_test_model());

    ecs::AnimatorSystem system;
    system.on_update(scene, 0.5f);

    EXPECT_FLOAT_EQ(mr->time, 0.5f);
    ASSERT_TRUE(mr->palette());
    const auto& palette = *mr->palette();
    ASSERT_EQ(palette.size(), 2u);

    // 与直接求值结果一致（bone1 t=0.5 平移 (0,1,0)，inverse_bind 为单位阵）
    auto expected = animation::evaluate_skin_palette(
        mr->model()->skeleton, mr->resolve_clip(), 0.5f, true);
    ASSERT_EQ(expected.size(), palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        for (int k = 0; k < 16; ++k) {
            EXPECT_NEAR(palette[i].m[k], expected[i].m[k], 1e-5f) << "bone " << i;
        }
    }
    // bone1 palette 的平移分量 ≈ (0,1,0)
    EXPECT_NEAR(palette[1](1, 3), 1.0f, 1e-5f);
}

TEST(AnimatorSystemTest, PaletteCappedAtGpuBoneLimit) {
    auto model = make_test_model();
    // 构造超上限骨架：129 骨链（bind 全单位）
    model->skeleton.bones.clear();
    for (int i = 0; i < 129; ++i) {
        animation::Bone b;
        b.name = "b" + std::to_string(i);
        b.parent_index = i - 1;
        b.inverse_bind_matrix = math::Matrix4f::identity();
        model->skeleton.bones.push_back(b);
    }
    model->animations.clear(); // clip == nullptr → bind pose palette

    scene::Scene scene("anim_cap_test");
    scene::Entity* e = scene.create_entity("actor");
    auto* mr = e->add_component<components::SkinnedMeshRenderer>("cap_model");
    mr->set_model(model);

    ecs::AnimatorSystem system;
    system.on_update(scene, 0.0f);

    ASSERT_TRUE(mr->palette());
    EXPECT_EQ(mr->palette()->size(), render::k_max_skinning_bones);
}

TEST(AnimatorSystemTest, DisabledRendererSkipped) {
    scene::Scene scene("anim_disabled_test");
    scene::Entity* e = scene.create_entity("actor");
    auto* mr = e->add_component<components::SkinnedMeshRenderer>("test_model");
    mr->set_model(make_test_model());
    mr->enabled = false;

    ecs::AnimatorSystem system;
    system.on_update(scene, 0.5f);

    EXPECT_FLOAT_EQ(mr->time, 0.0f);
    EXPECT_FALSE(mr->palette());
}

TEST(AnimatorSystemTest, NotPlayingKeepsTimeButStillEvaluates) {
    scene::Scene scene("anim_pause_test");
    scene::Entity* e = scene.create_entity("actor");
    auto* mr = e->add_component<components::SkinnedMeshRenderer>("test_model");
    mr->set_model(make_test_model());
    mr->playing = false;
    mr->time = 0.25f;

    ecs::AnimatorSystem system;
    system.on_update(scene, 0.5f);

    EXPECT_FLOAT_EQ(mr->time, 0.25f);
    ASSERT_TRUE(mr->palette());
    // t=0.25 → bone1 平移 y ≈ 0.5
    EXPECT_NEAR((*mr->palette())[1](1, 3), 0.5f, 1e-5f);
}
