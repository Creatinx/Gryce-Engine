// tests/ray_test.cpp
// M1-E2：射线/AABB 求交、NDC 反投影射线、四元数旋转矩阵互转

#include <gtest/gtest.h>

#include "math/ray.h"
#include "math/math.h"

using namespace gryce_engine::math;

// ---------------------------------------------------------------------------
// ray_intersect_aabb
// ---------------------------------------------------------------------------
TEST(MathRay, AabbHitFrontFace) {
    Ray ray{Vector3f(0.0f, 0.0f, 5.0f), Vector3f(0.0f, 0.0f, -1.0f)};
    float t = 0.0f;
    ASSERT_TRUE(ray_intersect_aabb(ray, Vector3f(-1.0f, -1.0f, -1.0f),
                                   Vector3f(1.0f, 1.0f, 1.0f), t));
    EXPECT_NEAR(t, 4.0f, 1e-4f);
}

TEST(MathRay, AabbMiss) {
    Ray ray{Vector3f(0.0f, 5.0f, 5.0f), Vector3f(0.0f, 0.0f, -1.0f)};
    float t = 0.0f;
    EXPECT_FALSE(ray_intersect_aabb(ray, Vector3f(-1.0f, -1.0f, -1.0f),
                                    Vector3f(1.0f, 1.0f, 1.0f), t));
}

TEST(MathRay, AabbOriginInside) {
    Ray ray{Vector3f(0.0f, 0.0f, 0.0f), Vector3f(1.0f, 0.0f, 0.0f)};
    float t = 0.0f;
    ASSERT_TRUE(ray_intersect_aabb(ray, Vector3f(-1.0f, -1.0f, -1.0f),
                                   Vector3f(1.0f, 1.0f, 1.0f), t));
    EXPECT_LE(t, 0.0f); // 起点在盒内，tmin 为负
}

TEST(MathRay, AabbBehindRay) {
    Ray ray{Vector3f(0.0f, 0.0f, 5.0f), Vector3f(0.0f, 0.0f, 1.0f)};
    float t = 0.0f;
    EXPECT_FALSE(ray_intersect_aabb(ray, Vector3f(-1.0f, -1.0f, -1.0f),
                                    Vector3f(1.0f, 1.0f, 1.0f), t));
}

TEST(MathRay, AabbParallelAxisOutsideSlab) {
    // 射线与 X 轴平行但 x 在 slab 外
    Ray ray{Vector3f(5.0f, 0.0f, 5.0f), Vector3f(0.0f, 0.0f, -1.0f)};
    float t = 0.0f;
    EXPECT_FALSE(ray_intersect_aabb(ray, Vector3f(-1.0f, -1.0f, -1.0f),
                                    Vector3f(1.0f, 1.0f, 1.0f), t));
}

// ---------------------------------------------------------------------------
// screen_ndc_to_ray（GL 风格 perspective，NDC z ∈ [-1,1]）
// ---------------------------------------------------------------------------
TEST(MathRay, CenterRayMatchesCameraForward) {
    // 构造 GL 风格 proj * view，中心射线应穿过 look_at 目标点
    const Matrix4f proj = Matrix4f::perspective(to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Matrix4f view = Matrix4f::look_at(Vector3f(0.0f, 0.0f, 5.0f),
                                            Vector3f(0.0f, 0.0f, 0.0f),
                                            Vector3f(0.0f, 1.0f, 0.0f));
    const Matrix4f inv_vp = (proj * view).inverse();

    const Ray ray = screen_ndc_to_ray(0.0f, 0.0f, inv_vp);
    // 视线穿过原点：射线上应存在 t 使 ray.at(t) ≈ (0,0,0)
    const float t = (Vector3f(0.0f, 0.0f, 0.0f) - ray.origin).dot(ray.direction);
    const Vector3f closest = ray.at(t);
    EXPECT_NEAR(closest.x, 0.0f, 1e-3f);
    EXPECT_NEAR(closest.y, 0.0f, 1e-3f);
    EXPECT_NEAR(closest.z, 0.0f, 1e-3f);
}

TEST(MathRay, RayProjectsBackToNdc) {
    const Matrix4f proj = Matrix4f::perspective(to_radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Matrix4f view = Matrix4f::look_at(Vector3f(1.0f, 2.0f, 5.0f),
                                            Vector3f(0.0f, 0.0f, 0.0f),
                                            Vector3f(0.0f, 1.0f, 0.0f));
    const Matrix4f vp = proj * view;
    const Matrix4f inv_vp = vp.inverse();

    // 取 NDC (0.25, -0.5) 生成射线，射线上任一点投影回去应得到相同 NDC x/y
    const Ray ray = screen_ndc_to_ray(0.25f, -0.5f, inv_vp);
    const Vector3f world = ray.at(10.0f);
    const Vector4f clip = vp * Vector4f(world.x, world.y, world.z, 1.0f);
    ASSERT_GT(std::abs(clip.w), 1e-6f);
    EXPECT_NEAR(clip.x / clip.w, 0.25f, 1e-3f);
    EXPECT_NEAR(clip.y / clip.w, -0.5f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Quaternionf::from_rotation_matrix（M1-E2 gizmo 矩阵分解用）
// ---------------------------------------------------------------------------
TEST(MathQuaternion, FromRotationMatrixRoundtrip) {
    const Quaternionf q1 = Quaternionf::from_euler(0.3f, -1.2f, 0.7f).normalized();
    const Quaternionf q2 = Quaternionf::from_rotation_matrix(q1.to_matrix());

    // 四元数有 q/-q 二义性，用旋转向量比较
    const Vector3f v(0.3f, -0.7f, 1.1f);
    const Vector3f a = q1.rotate_vector(v);
    const Vector3f b = q2.rotate_vector(v);
    EXPECT_NEAR(a.x, b.x, 1e-4f);
    EXPECT_NEAR(a.y, b.y, 1e-4f);
    EXPECT_NEAR(a.z, b.z, 1e-4f);
}

TEST(MathQuaternion, FromRotationMatrixAxisDominant) {
    // 覆盖 Shepperd 法的各分支（大旋转角使 trace 为负）
    const Quaternionf q1 = Quaternionf::from_axis_angle(
        Vector3f(1.0f, 0.0f, 0.0f), to_radians(170.0f));
    const Quaternionf q2 = Quaternionf::from_rotation_matrix(q1.to_matrix());
    const Vector3f v(0.1f, 0.9f, -0.3f);
    const Vector3f a = q1.rotate_vector(v);
    const Vector3f b = q2.rotate_vector(v);
    EXPECT_NEAR(a.x, b.x, 1e-4f);
    EXPECT_NEAR(a.y, b.y, 1e-4f);
    EXPECT_NEAR(a.z, b.z, 1e-4f);
}
