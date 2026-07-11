#include <gtest/gtest.h>
#include "math/math.h"

using namespace gryce_engine::math;

// ---------------------------------------------------------------------------
// Vector3f
// ---------------------------------------------------------------------------
TEST(MathVector3, Construction) {
    Vector3f v(1, 2, 3);
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.y, 2.0f);
    EXPECT_FLOAT_EQ(v.z, 3.0f);
}

TEST(MathVector3, DotProduct) {
    Vector3f a(1, 2, 3);
    Vector3f b(4, 5, 6);
    EXPECT_FLOAT_EQ(a.dot(b), 32.0f);
}

TEST(MathVector3, CrossProduct) {
    Vector3f a(1, 0, 0);
    Vector3f b(0, 1, 0);
    Vector3f c = a.cross(b);
    EXPECT_FLOAT_EQ(c.x, 0.0f);
    EXPECT_FLOAT_EQ(c.y, 0.0f);
    EXPECT_FLOAT_EQ(c.z, 1.0f);
}

TEST(MathVector3, LengthAndNormalize) {
    Vector3f v(3, 4, 0);
    EXPECT_FLOAT_EQ(v.length(), 5.0f);
    EXPECT_FLOAT_EQ(v.length_sq(), 25.0f);
    EXPECT_FLOAT_EQ(v.normalized().length(), 1.0f);
}

TEST(MathVector3, CompoundOperators) {
    Vector3f v(1, 2, 3);
    v += Vector3f(1, 1, 1);
    EXPECT_FLOAT_EQ(v.x, 2.0f);
    EXPECT_FLOAT_EQ(v.y, 3.0f);
    EXPECT_FLOAT_EQ(v.z, 4.0f);
}

TEST(MathVector3, Equality) {
    Vector3f a(1, 2, 3);
    Vector3f b(1, 2, 3);
    Vector3f c(1, 2, 4);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

// ---------------------------------------------------------------------------
// Matrix4f
// ---------------------------------------------------------------------------
TEST(MathMatrix4, Identity) {
    auto m = Matrix4f::identity();
    Vector3f v(1, 2, 3);
    Vector3f r = m.transform_point(v);
    EXPECT_FLOAT_EQ(r.x, 1.0f);
    EXPECT_FLOAT_EQ(r.y, 2.0f);
    EXPECT_FLOAT_EQ(r.z, 3.0f);
}

TEST(MathMatrix4, Multiply) {
    auto a = Matrix4f::from_diagonal(2, 3, 4);
    auto b = Matrix4f::from_diagonal(5, 6, 7);
    auto c = a * b;
    Vector3f v(1, 1, 1);
    Vector3f r = c.transform_point(v);
    EXPECT_FLOAT_EQ(r.x, 10.0f);
    EXPECT_FLOAT_EQ(r.y, 18.0f);
    EXPECT_FLOAT_EQ(r.z, 28.0f);
}

TEST(MathMatrix4, ColumnMajor) {
    auto m = Matrix4f::identity();
    EXPECT_FLOAT_EQ(m(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(m(1, 1), 1.0f);
    EXPECT_FLOAT_EQ(m(2, 2), 1.0f);
    EXPECT_FLOAT_EQ(m(3, 3), 1.0f);
    EXPECT_FLOAT_EQ(m(0, 1), 0.0f);
    EXPECT_FLOAT_EQ(m(1, 0), 0.0f);
}

TEST(MathMatrix4, Transpose) {
    auto m = Matrix4f::identity();
    m(0, 1) = 2.0f;
    m(1, 0) = 3.0f;
    auto t = m.transpose();
    EXPECT_FLOAT_EQ(t(0, 1), 3.0f);
    EXPECT_FLOAT_EQ(t(1, 0), 2.0f);
}

TEST(MathMatrix4, Inverse) {
    auto m = Matrix4f::translate(1, 2, 3);
    auto inv = m.inverse();
    Vector3f v(4, 5, 6);
    Vector3f r = inv.transform_point(m.transform_point(v));
    EXPECT_NEAR(r.x, v.x, 1e-4f);
    EXPECT_NEAR(r.y, v.y, 1e-4f);
    EXPECT_NEAR(r.z, v.z, 1e-4f);
}

TEST(MathMatrix4, InverseRotation) {
    auto m = Matrix4f::rotate(to_radians(45.0f), Vector3f(0, 0, 1));
    auto inv = m.inverse();
    Vector3f v(1, 0, 0);
    Vector3f r = inv.transform_point(m.transform_point(v));
    EXPECT_NEAR(r.x, v.x, 1e-4f);
    EXPECT_NEAR(r.y, v.y, 1e-4f);
    EXPECT_NEAR(r.z, v.z, 1e-4f);
}

TEST(MathMatrix4, Translate) {
    auto m = Matrix4f::translate(1, 2, 3);
    Vector3f v(0, 0, 0);
    Vector3f r = m.transform_point(v);
    EXPECT_FLOAT_EQ(r.x, 1.0f);
    EXPECT_FLOAT_EQ(r.y, 2.0f);
    EXPECT_FLOAT_EQ(r.z, 3.0f);
}

TEST(MathMatrix4, Scale) {
    auto m = Matrix4f::scale(2, 3, 4);
    Vector3f v(1, 1, 1);
    Vector3f r = m.transform_point(v);
    EXPECT_FLOAT_EQ(r.x, 2.0f);
    EXPECT_FLOAT_EQ(r.y, 3.0f);
    EXPECT_FLOAT_EQ(r.z, 4.0f);
}

TEST(MathMatrix4, Rotate) {
    auto m = Matrix4f::rotate(to_radians(90.0f), Vector3f(0, 0, 1));
    Vector3f v(1, 0, 0);
    Vector3f r = m.transform_point(v);
    EXPECT_NEAR(r.x, 0.0f, 1e-4f);
    EXPECT_NEAR(r.y, 1.0f, 1e-4f);
    EXPECT_NEAR(r.z, 0.0f, 1e-4f);
}

TEST(MathMatrix4, LookAt) {
    Vector3f eye(0, 0, 5);
    Vector3f center(0, 0, 0);
    Vector3f up(0, 1, 0);
    auto view = Matrix4f::look_at(eye, center, up);
    Vector3f world_pos(0, 0, 0);
    Vector3f view_pos = view.transform_point(world_pos);
    // (0,0,0) 在 view space 中应在 (0,0,-5) 处（因为 eye 在 z=5，看向 z=0）
    EXPECT_NEAR(view_pos.x, 0.0f, 1e-4f);
    EXPECT_NEAR(view_pos.y, 0.0f, 1e-4f);
    EXPECT_NEAR(view_pos.z, -5.0f, 1e-4f);
}

TEST(MathMatrix4, Perspective) {
    float fov = to_radians(90.0f);
    float aspect = 16.0f / 9.0f;
    auto proj = Matrix4f::perspective(fov, aspect, 0.1f, 100.0f);
    // near 平面上的点在 clip space 中 z 应该接近 -1
    Vector3f near_point(0, 0, -0.1f);
    Vector4f clip = proj * Vector4f(near_point, 1.0f);
    EXPECT_NEAR(clip.z / clip.w, -1.0f, 1e-3f);
    // far 平面上的点在 clip space 中 z 应该接近 1
    Vector3f far_point(0, 0, -100.0f);
    clip = proj * Vector4f(far_point, 1.0f);
    EXPECT_NEAR(clip.z / clip.w, 1.0f, 1e-3f);
}

TEST(MathMatrix4, Ortho) {
    auto proj = Matrix4f::ortho(-1, 1, -1, 1, 0.1f, 100.0f);
    Vector3f v(0, 0, -0.1f);
    Vector3f r = proj.transform_point(v);
    EXPECT_NEAR(r.z, -1.0f, 1e-4f);
    Vector3f v2(0, 0, -100.0f);
    r = proj.transform_point(v2);
    EXPECT_NEAR(r.z, 1.0f, 1e-4f);
}

TEST(MathMatrix4, ExtractVectors) {
    auto m = Matrix4f::look_at(Vector3f(0, 0, 5), Vector3f(0, 0, 0), Vector3f(0, 1, 0));
    Vector3f right = m.right_vector();
    Vector3f up = m.up_vector();
    Vector3f forward = m.forward_vector();
    EXPECT_NEAR(right.dot(up), 0.0f, 1e-4f);
    EXPECT_NEAR(right.dot(forward), 0.0f, 1e-4f);
    EXPECT_NEAR(up.dot(forward), 0.0f, 1e-4f);
}

TEST(MathMatrix4, ModelMatrix) {
    auto model = Matrix4f::translate(1, 2, 3) * Matrix4f::rotate(to_radians(90.0f), Vector3f(0, 0, 1)) * Matrix4f::scale(2, 2, 2);
    Vector3f v(1, 0, 0);
    Vector3f r = model.transform_point(v);
    // scale: (2,0,0) -> rotate: (0,2,0) -> translate: (1, 4, 3)
    EXPECT_NEAR(r.x, 1.0f, 1e-4f);
    EXPECT_NEAR(r.y, 4.0f, 1e-4f);
    EXPECT_NEAR(r.z, 3.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Vector3f — 新增工具函数
// ---------------------------------------------------------------------------
TEST(MathVector3, Lerp) {
    Vector3f a(0, 0, 0);
    Vector3f b(1, 2, 3);
    Vector3f r = a.lerp(b, 0.5f);
    EXPECT_FLOAT_EQ(r.x, 0.5f);
    EXPECT_FLOAT_EQ(r.y, 1.0f);
    EXPECT_FLOAT_EQ(r.z, 1.5f);
}

TEST(MathVector3, Clamp) {
    Vector3f v(5, -2, 0.5f);
    Vector3f r = v.clamp(0.0f, 1.0f);
    EXPECT_FLOAT_EQ(r.x, 1.0f);
    EXPECT_FLOAT_EQ(r.y, 0.0f);
    EXPECT_FLOAT_EQ(r.z, 0.5f);
}

TEST(MathVector3, Distance) {
    Vector3f a(0, 0, 0);
    Vector3f b(3, 4, 0);
    EXPECT_FLOAT_EQ(a.distance(b), 5.0f);
    EXPECT_FLOAT_EQ(a.distance_sq(b), 25.0f);
}

TEST(MathVector3, StaticAxes) {
    EXPECT_EQ(Vector3f::up(), Vector3f(0, 1, 0));
    EXPECT_EQ(Vector3f::right(), Vector3f(1, 0, 0));
    EXPECT_EQ(Vector3f::forward(), Vector3f(0, 0, -1));
    EXPECT_EQ(Vector3f::zero(), Vector3f(0, 0, 0));
    EXPECT_EQ(Vector3f::one(), Vector3f(1, 1, 1));
}

TEST(MathVector3, ToolFunctions) {
    Vector3f a(-1, 2, -3);
    Vector3f b = a.abs();
    EXPECT_FLOAT_EQ(b.x, 1.0f);
    EXPECT_FLOAT_EQ(b.y, 2.0f);
    EXPECT_FLOAT_EQ(b.z, 3.0f);

    Vector3f c(1, 5, 3);
    Vector3f d(2, 4, 6);
    Vector3f mn = c.min(d);
    Vector3f mx = c.max(d);
    EXPECT_FLOAT_EQ(mn.x, 1.0f);
    EXPECT_FLOAT_EQ(mn.y, 4.0f);
    EXPECT_FLOAT_EQ(mn.z, 3.0f);
    EXPECT_FLOAT_EQ(mx.x, 2.0f);
    EXPECT_FLOAT_EQ(mx.y, 5.0f);
    EXPECT_FLOAT_EQ(mx.z, 6.0f);
}

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
TEST(MathTools, RadiansDegrees) {
    EXPECT_NEAR(to_radians(180.0f), 3.14159265f, 1e-5f);
    EXPECT_NEAR(to_radians(90.0f), 3.14159265f / 2.0f, 1e-5f);
    EXPECT_NEAR(to_degrees(3.14159265f), 180.0f, 1e-4f);
    EXPECT_NEAR(to_degrees(3.14159265f / 2.0f), 90.0f, 1e-4f);
}

TEST(MathTools, LerpSaturateClamp) {
    EXPECT_FLOAT_EQ(lerp(0.0f, 10.0f, 0.5f), 5.0f);
    EXPECT_FLOAT_EQ(clamp(5.0f, 0.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(clamp(-1.0f, 0.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(saturate(1.5f), 1.0f);
    EXPECT_FLOAT_EQ(saturate(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(saturate(0.5f), 0.5f);
}

// ---------------------------------------------------------------------------
// Quaternionf
// ---------------------------------------------------------------------------
TEST(MathQuaternion, Identity) {
    auto q = Quaternionf::identity();
    EXPECT_FLOAT_EQ(q.w, 1.0f);
    EXPECT_FLOAT_EQ(q.x, 0.0f);
    EXPECT_FLOAT_EQ(q.y, 0.0f);
    EXPECT_FLOAT_EQ(q.z, 0.0f);
}

TEST(MathQuaternion, AxisAngle) {
    auto q = Quaternionf::from_axis_angle(Vector3f(0, 0, 1), 3.14159265f);
    auto m = q.to_matrix();
    Vector3f v(1, 0, 0);
    Vector3f r = m.transform_point(v);
    EXPECT_NEAR(r.x, -1.0f, 1e-3f);
    EXPECT_NEAR(r.y, 0.0f, 1e-3f);
    EXPECT_NEAR(r.z, 0.0f, 1e-3f);
}

TEST(MathQuaternion, EulerAngles) {
    auto q = Quaternionf::from_euler(0.0f, 3.14159265f / 2.0f, 0.0f);
    Vector3f v(1, 0, 0);
    Vector3f r = q.rotate_vector(v);
    EXPECT_NEAR(r.x, 0.0f, 1e-3f);
    EXPECT_NEAR(r.y, 1.0f, 1e-3f);
    EXPECT_NEAR(r.z, 0.0f, 1e-3f);
}

TEST(MathQuaternion, HamiltonProduct) {
    auto q1 = Quaternionf::from_axis_angle(Vector3f(0, 0, 1), 3.14159265f / 2.0f);
    auto q2 = Quaternionf::from_axis_angle(Vector3f(0, 0, 1), 3.14159265f / 2.0f);
    auto q = q1 * q2;
    Vector3f v(1, 0, 0);
    Vector3f r = q.rotate_vector(v);
    EXPECT_NEAR(r.x, -1.0f, 1e-3f);
    EXPECT_NEAR(r.y, 0.0f, 1e-3f);
}

TEST(MathQuaternion, Conjugate) {
    Quaternionf q(1, 2, 3, 4);
    auto c = q.conjugate();
    EXPECT_FLOAT_EQ(c.x, -1.0f);
    EXPECT_FLOAT_EQ(c.y, -2.0f);
    EXPECT_FLOAT_EQ(c.z, -3.0f);
    EXPECT_FLOAT_EQ(c.w, 4.0f);
}
