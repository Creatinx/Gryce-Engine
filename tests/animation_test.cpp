#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "animation/animation_clip.h"
#include "animation/pose.h"
#include "animation/skeleton.h"
#include "assets/skinned_mesh_data.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#ifdef GRYCE_HAS_ASSIMP
#include "assets/assimp_importer.h"
#endif

using namespace gryce_engine;

namespace {

// 矩阵逐元素近似比较
void expect_matrix_near(const math::Matrix4f& a, const math::Matrix4f& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(a.m[i], b.m[i], eps) << "m[" << i << "]";
    }
}

void expect_vector_near(const math::Vector3f& a, const math::Vector3f& b, float eps = 1e-4f) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

// 构造 3 骨骼链：root T(1,0,0) → child T(0,1,0) → grandchild T(0,0,1)，
// inverse bind 由 bind pose 的 global 求逆（标准做法）。
animation::Skeleton make_three_bone_skeleton() {
    animation::Skeleton skel;

    animation::Bone root;
    root.name = "root";
    root.parent_index = -1;
    root.bind_position = math::Vector3f(1, 0, 0);

    animation::Bone child;
    child.name = "child";
    child.parent_index = 0;
    child.bind_position = math::Vector3f(0, 1, 0);

    animation::Bone grandchild;
    grandchild.name = "grandchild";
    grandchild.parent_index = 1;
    grandchild.bind_position = math::Vector3f(0, 0, 1);

    skel.bones = {root, child, grandchild};

    auto local = animation::sample_local_pose(skel, nullptr, 0.0f);
    auto global = animation::compute_global_pose(skel, local);
    for (size_t i = 0; i < 3; ++i) {
        skel.bones[i].inverse_bind_matrix = global[i].inverse();
    }
    return skel;
}

} // namespace

// ---------------------------------------------------------------------------
// Quaternion slerp
// ---------------------------------------------------------------------------

TEST(AnimationSlerp, HalfwayRotation) {
    // identity → 绕 Y 轴 90°，t=0.5 应为绕 Y 轴 45°。
    // （不用 180° 端点：两端点 4D 夹角 ~180° 时两条短弧等长，
    //   float 精度决定的翻转方向会让中点落在 ±90° 的任一侧，属实现合理行为）
    const auto a = math::Quaternionf::identity();
    const auto b = math::Quaternionf::from_axis_angle(math::Vector3f::up(), 3.14159265f * 0.5f);
    const auto mid = math::Quaternionf::slerp(a, b, 0.5f);

    const math::Vector3f rotated = mid.rotate_vector(math::Vector3f::right());
    // 绕 +Y 旋转 45°：+X → (cos45, 0, -sin45)
    const float s45 = 0.70710678f;
    expect_vector_near(rotated, math::Vector3f(s45, 0, -s45), 1e-3f);
}

TEST(AnimationSlerp, ShortestPathFlipsSign) {
    // q 与 -q 表示同一旋转；dot<0 必须翻转走短弧，结果保持在原旋转附近
    const auto q = math::Quaternionf::from_axis_angle(math::Vector3f::up(), 1.0f);
    const auto neg_q = math::Quaternionf(-q.x, -q.y, -q.z, -q.w);
    const auto r = math::Quaternionf::slerp(q, neg_q, 0.5f);

    expect_vector_near(r.rotate_vector(math::Vector3f::right()),
                       q.rotate_vector(math::Vector3f::right()), 1e-3f);
}

TEST(AnimationSlerp, NearlyIdenticalFallsBackToNlerp) {
    // 几乎重合的两个四元数：sinθ→0，应退化 nlerp 且不产生 NaN
    const auto a = math::Quaternionf::from_axis_angle(math::Vector3f::up(), 0.5f);
    const auto b = math::Quaternionf::from_axis_angle(math::Vector3f::up(), 0.500001f);
    const auto r = math::Quaternionf::slerp(a, b, 0.5f);

    EXPECT_NEAR(r.length(), 1.0f, 1e-4f);
    EXPECT_FALSE(std::isnan(r.x) || std::isnan(r.w));
}

// ---------------------------------------------------------------------------
// AnimationClip 关键帧插值
// ---------------------------------------------------------------------------

TEST(AnimationClipTest, PositionLerp) {
    animation::AnimationClip clip;
    clip.duration = 1.0f;

    animation::BoneTrack track;
    track.bone_index = 0;
    track.position_keys = {{0.0f, math::Vector3f(0, 0, 0)},
                           {1.0f, math::Vector3f(10, 0, 0)}};
    clip.tracks.push_back(track);

    const math::Vector3f bind(0, 0, 0);
    const auto m = clip.sample_bone(0, 0.25f, false, bind, math::Quaternionf::identity(),
                                    math::Vector3f::one());
    expect_vector_near(m.translation(), math::Vector3f(2.5f, 0, 0));
}

TEST(AnimationClipTest, ClampOutOfRange) {
    animation::AnimationClip clip;
    clip.duration = 1.0f;

    animation::BoneTrack track;
    track.bone_index = 0;
    track.position_keys = {{0.0f, math::Vector3f(0, 0, 0)},
                           {1.0f, math::Vector3f(10, 0, 0)}};
    clip.tracks.push_back(track);

    const math::Vector3f bind(0, 0, 0);
    expect_vector_near(
        clip.sample_bone(0, -1.0f, false, bind, math::Quaternionf::identity(), math::Vector3f::one()).translation(),
        math::Vector3f(0, 0, 0));
    expect_vector_near(
        clip.sample_bone(0, 99.0f, false, bind, math::Quaternionf::identity(), math::Vector3f::one()).translation(),
        math::Vector3f(10, 0, 0));
}

TEST(AnimationClipTest, LoopWrapsTime) {
    animation::AnimationClip clip;
    clip.duration = 1.0f;

    animation::BoneTrack track;
    track.bone_index = 0;
    track.position_keys = {{0.0f, math::Vector3f(0, 0, 0)},
                           {1.0f, math::Vector3f(10, 0, 0)}};
    clip.tracks.push_back(track);

    const math::Vector3f bind(0, 0, 0);
    // 1.25s 循环等价 0.25s；负时间 -0.75s 循环等价 0.25s
    expect_vector_near(
        clip.sample_bone(0, 1.25f, true, bind, math::Quaternionf::identity(), math::Vector3f::one()).translation(),
        math::Vector3f(2.5f, 0, 0));
    expect_vector_near(
        clip.sample_bone(0, -0.75f, true, bind, math::Quaternionf::identity(), math::Vector3f::one()).translation(),
        math::Vector3f(2.5f, 0, 0));
}

TEST(AnimationClipTest, SingleKeyIsConstant) {
    animation::AnimationClip clip;
    clip.duration = 2.0f;

    animation::BoneTrack track;
    track.bone_index = 0;
    track.scale_keys = {{0.5f, math::Vector3f(2, 2, 2)}};
    clip.tracks.push_back(track);

    expect_vector_near(
        clip.sample_bone(0, 0.0f, false, math::Vector3f::zero(),
                         math::Quaternionf::identity(), math::Vector3f::one()).transform_vector(math::Vector3f::right()),
        math::Vector3f(2, 0, 0));
}

TEST(AnimationClipTest, RotationUsesSlerp) {
    animation::AnimationClip clip;
    clip.duration = 1.0f;

    animation::BoneTrack track;
    track.bone_index = 0;
    track.rotation_keys = {
        {0.0f, math::Quaternionf::identity()},
        {1.0f, math::Quaternionf::from_axis_angle(math::Vector3f::up(), 3.14159265f)}};
    clip.tracks.push_back(track);

    const auto m = clip.sample_bone(0, 0.5f, false, math::Vector3f::zero(),
                                    math::Quaternionf::identity(), math::Vector3f::one());
    // 矩阵列 0 即旋转后的 +X 方向：绕 +Y 90° 应为 -Z
    const math::Vector3f rotated_x = m.transform_vector(math::Vector3f::right());
    expect_vector_near(rotated_x, math::Vector3f(0, 0, -1), 1e-3f);
}

TEST(AnimationClipTest, MissingChannelFallsBackToBind) {
    animation::AnimationClip clip;
    clip.duration = 1.0f;

    // 轨道只有 position 通道：rotation/scale 必须回退 bind 值
    animation::BoneTrack track;
    track.bone_index = 0;
    track.position_keys = {{0.0f, math::Vector3f(5, 0, 0)},
                           {1.0f, math::Vector3f(5, 0, 0)}};
    clip.tracks.push_back(track);

    const auto bind_rot = math::Quaternionf::from_axis_angle(math::Vector3f::up(), 1.0f);
    const auto m = clip.sample_bone(0, 0.5f, false, math::Vector3f::zero(), bind_rot,
                                    math::Vector3f::one());
    expect_matrix_near(m,
                       math::Matrix4f::translate(math::Vector3f(5, 0, 0)) * bind_rot.to_matrix());
}

// ---------------------------------------------------------------------------
// BoneInfluence 权重截断与归一化
// ---------------------------------------------------------------------------

TEST(BoneInfluenceTest, NormalizeScalesToOne) {
    assets::BoneInfluence inf;
    inf.add(0, 1.0f);
    inf.add(1, 1.0f);
    inf.add(2, 2.0f);
    inf.normalize();

    EXPECT_NEAR(inf.weight_sum(), 1.0f, 1e-6f);
    EXPECT_NEAR(inf.weights[0], 0.5f, 1e-6f);   // 降序：2.0 → 0.5
    EXPECT_NEAR(inf.weights[1], 0.25f, 1e-6f);
    EXPECT_NEAR(inf.weights[2], 0.25f, 1e-6f);
    EXPECT_EQ(inf.bone_ids[0], 2);
}

TEST(BoneInfluenceTest, TruncatesToTopFour) {
    assets::BoneInfluence inf;
    inf.add(0, 0.05f);
    inf.add(1, 0.1f);
    inf.add(2, 0.2f);
    inf.add(3, 0.5f);
    inf.add(4, 0.4f);
    inf.add(5, 0.3f);

    // 只保留 0.5/0.4/0.3/0.2 四个最大权重（降序）
    EXPECT_NEAR(inf.weights[0], 0.5f, 1e-6f);
    EXPECT_NEAR(inf.weights[1], 0.4f, 1e-6f);
    EXPECT_NEAR(inf.weights[2], 0.3f, 1e-6f);
    EXPECT_NEAR(inf.weights[3], 0.2f, 1e-6f);
    EXPECT_EQ(inf.bone_ids[0], 3);
    EXPECT_EQ(inf.bone_ids[1], 4);
    EXPECT_EQ(inf.bone_ids[2], 5);
    EXPECT_EQ(inf.bone_ids[3], 2);

    inf.normalize();
    EXPECT_NEAR(inf.weight_sum(), 1.0f, 1e-6f);
}

TEST(BoneInfluenceTest, MergeSameBone) {
    assets::BoneInfluence inf;
    inf.add(1, 0.3f);
    inf.add(1, 0.5f);   // 同 bone 权重累加
    inf.add(2, 0.2f);

    EXPECT_NEAR(inf.weights[0], 0.8f, 1e-6f);
    EXPECT_EQ(inf.bone_ids[0], 1);
    EXPECT_NEAR(inf.weights[1], 0.2f, 1e-6f);
}

TEST(BoneInfluenceTest, ZeroWeightIgnored) {
    assets::BoneInfluence inf;
    inf.add(0, 0.0f);
    inf.add(1, -1.0f);
    EXPECT_NEAR(inf.weight_sum(), 0.0f, 1e-6f);
    inf.normalize();    // 全零不应产生 NaN
    EXPECT_NEAR(inf.weight_sum(), 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Skeleton
// ---------------------------------------------------------------------------

TEST(SkeletonTest, FindBone) {
    animation::Skeleton skel = make_three_bone_skeleton();
    EXPECT_EQ(skel.find_bone("root"), 0);
    EXPECT_EQ(skel.find_bone("child"), 1);
    EXPECT_EQ(skel.find_bone("grandchild"), 2);
    EXPECT_EQ(skel.find_bone("no_such_bone"), -1);
}

TEST(SkeletonTest, HierarchyValidation) {
    animation::Skeleton skel = make_three_bone_skeleton();
    EXPECT_TRUE(skel.is_valid_hierarchy());

    // 拓扑序被破坏（父下标 >= 自身下标）应判非法
    animation::Skeleton bad;
    animation::Bone b0;
    b0.name = "a";
    b0.parent_index = 1;    // 指向自己之后的下标
    animation::Bone b1;
    b1.name = "b";
    b1.parent_index = -1;
    bad.bones = {b0, b1};
    EXPECT_FALSE(bad.is_valid_hierarchy());
}

// ---------------------------------------------------------------------------
// Pose 求值（手工验证 3 骨骼层级）
// ---------------------------------------------------------------------------

TEST(PoseEvaluation, BindPosePaletteIsIdentity) {
    // inverse bind 由 global bind 求逆时，bind pose 的 palette 恒等于单位阵
    animation::Skeleton skel = make_three_bone_skeleton();
    auto palette = animation::evaluate_skin_palette(skel, nullptr, 0.0f);
    ASSERT_EQ(palette.size(), 3);
    for (const auto& m : palette) {
        expect_matrix_near(m, math::Matrix4f::identity());
    }
}

TEST(PoseEvaluation, GlobalPoseAccumulatesParentTransform) {
    animation::Skeleton skel = make_three_bone_skeleton();
    auto local = animation::sample_local_pose(skel, nullptr, 0.0f);
    auto global = animation::compute_global_pose(skel, local);
    ASSERT_EQ(global.size(), 3);

    // root T(1,0,0) → child 继承 → grandchild 再继承
    expect_vector_near(global[0].translation(), math::Vector3f(1, 0, 0));
    expect_vector_near(global[1].translation(), math::Vector3f(1, 1, 0));
    expect_vector_near(global[2].translation(), math::Vector3f(1, 1, 1));
}

TEST(PoseEvaluation, AnimatedTranslationPropagatesToDescendants) {
    animation::Skeleton skel = make_three_bone_skeleton();

    // 动画：child 的 local 平移从 (0,1,0) 变为 (0,2,0)
    animation::AnimationClip clip;
    clip.duration = 1.0f;
    animation::BoneTrack track;
    track.bone_index = skel.find_bone("child");
    track.position_keys = {{0.0f, math::Vector3f(0, 1, 0)},
                           {1.0f, math::Vector3f(0, 2, 0)}};
    clip.tracks.push_back(track);

    auto palette = animation::evaluate_skin_palette(skel, &clip, 1.0f, false);
    ASSERT_EQ(palette.size(), 3);

    // child 的 global 变为 T(1,2,0)，palette = T(1,2,0) * inverse(T(1,1,0)) = T(0,1,0)
    expect_vector_near(palette[1].translation(), math::Vector3f(0, 1, 0));
    // grandchild 未动画但随父级移动：palette 同样是 T(0,1,0)
    expect_vector_near(palette[2].translation(), math::Vector3f(0, 1, 0));
    // root 不受影响
    expect_vector_near(palette[0].translation(), math::Vector3f(0, 0, 0));

    // 蒙皮语义验证：bind pose 下的原点经 child palette 变换后应 +1 Y
    expect_vector_near(palette[1].transform_point(math::Vector3f::zero()),
                       math::Vector3f(0, 1, 0));
}

// ---------------------------------------------------------------------------
// Assimp skin 导入（集成测试：需要 skinned_triangle.gltf fixture）
// ---------------------------------------------------------------------------

#ifdef GRYCE_HAS_ASSIMP
TEST(SkinnedImportTest, ImportSkinnedGltf) {
    const std::string path =
        std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures/models/skinned_triangle.gltf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "skinned_triangle.gltf fixture 不存在（tools/gen_skinned_fixture.py 生成）";
    }

    assets::AssimpImporter importer;
    auto model = importer.import_skinned(path);

    // --- skeleton ---
    ASSERT_TRUE(model.has_skin);
    const auto& skel = model.skeleton;
    const int32_t j0 = skel.find_bone("joint0");
    const int32_t j1 = skel.find_bone("joint1");
    ASSERT_GE(j0, 0);
    ASSERT_GE(j1, 0);
    EXPECT_TRUE(skel.is_valid_hierarchy());
    // joint1 的父级是 joint0
    EXPECT_EQ(skel.bones[j1].parent_index, j0);
    // joint1 的 bind 平移来自节点 translation
    expect_vector_near(skel.bones[j1].bind_position, math::Vector3f(0, 1, 0), 1e-3f);

    // --- mesh 与顶点权重 ---
    ASSERT_EQ(model.meshes.size(), 1);
    const auto& mesh = model.meshes[0];
    ASSERT_TRUE(mesh.has_skin());
    ASSERT_EQ(mesh.bone_influences.size(), 3);

    // v0：全 joint0
    EXPECT_EQ(mesh.bone_influences[0].bone_ids[0], static_cast<uint16_t>(j0));
    EXPECT_NEAR(mesh.bone_influences[0].weights[0], 1.0f, 1e-4f);
    // v1：全 joint1
    EXPECT_EQ(mesh.bone_influences[1].bone_ids[0], static_cast<uint16_t>(j1));
    EXPECT_NEAR(mesh.bone_influences[1].weights[0], 1.0f, 1e-4f);
    // v2：joint0/joint1 各半（归一化后和为 1）
    EXPECT_NEAR(mesh.bone_influences[2].weight_sum(), 1.0f, 1e-4f);
    EXPECT_NEAR(mesh.bone_influences[2].weights[0], 0.5f, 1e-4f);
    EXPECT_NEAR(mesh.bone_influences[2].weights[1], 0.5f, 1e-4f);

    // --- 动画剪辑 ---
    ASSERT_EQ(model.animations.size(), 1);
    const auto& clip = model.animations[0];
    EXPECT_NEAR(clip.duration, 1.0f, 1e-3f);
    EXPECT_NE(clip.find_track(j1), nullptr);
    EXPECT_EQ(clip.find_track(j0), nullptr);

    // t=1.0 时 joint1 local 平移 = (0,3,0)，global = (0,3,0)，
    // palette = T(0,3,0) * T(0,-1,0) = T(0,2,0)
    auto palette = animation::evaluate_skin_palette(skel, &clip, 1.0f, false);
    ASSERT_EQ(palette.size(), skel.bone_count());
    expect_vector_near(palette[j1].translation(), math::Vector3f(0, 2, 0), 1e-3f);

    // t=0.5 时线性插值到 (0,2,0)，palette = T(0,1,0)
    auto palette_mid = animation::evaluate_skin_palette(skel, &clip, 0.5f, false);
    expect_vector_near(palette_mid[j1].translation(), math::Vector3f(0, 1, 0), 1e-3f);
}

TEST(SkinnedImportTest, NonSkinnedModelHasNoSkin) {
    // 普通 OBJ（无蒙皮）：import_skinned 应退化为普通网格
    const std::string path =
        std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures/models/cube.obj";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "cube.obj fixture 不存在";
    }

    assets::AssimpImporter importer;
    auto model = importer.import_skinned(path);
    EXPECT_FALSE(model.has_skin);
    ASSERT_FALSE(model.meshes.empty());
    EXPECT_FALSE(model.meshes[0].has_skin());
}
#endif
