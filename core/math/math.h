#pragma once

#include <cmath>
#include <algorithm>

namespace gryce_engine::math {

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
constexpr float to_radians(float deg) { return deg * 3.14159265358979323846f / 180.0f; }
constexpr float to_degrees(float rad) { return rad * 180.0f / 3.14159265358979323846f; }
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float clamp(float v, float min_val, float max_val) {
    return v < min_val ? min_val : (v > max_val ? max_val : v);
}
constexpr float saturate(float v) { return clamp(v, 0.0f, 1.0f); }

// ---------------------------------------------------------------------------
// Vector2f
// ---------------------------------------------------------------------------
struct Vector2f {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vector2f() = default;
    constexpr Vector2f(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vector2f operator+(const Vector2f& o) const { return Vector2f(x + o.x, y + o.y); }
    constexpr Vector2f operator-(const Vector2f& o) const { return Vector2f(x - o.x, y - o.y); }
    constexpr Vector2f operator*(float s) const { return Vector2f(x * s, y * s); }
    constexpr Vector2f operator/(float s) const { return Vector2f(x / s, y / s); }

    constexpr Vector2f& operator+=(const Vector2f& o) { x += o.x; y += o.y; return *this; }
    constexpr Vector2f& operator-=(const Vector2f& o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vector2f& operator*=(float s) { x *= s; y *= s; return *this; }
    constexpr Vector2f& operator/=(float s) { x /= s; y /= s; return *this; }

    constexpr float dot(const Vector2f& o) const { return x * o.x + y * o.y; }
    constexpr float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    Vector2f normalized() const { return *this / length(); }

    Vector2f lerp(const Vector2f& o, float t) const;
    Vector2f clamp(float min_val, float max_val) const;
    Vector2f abs() const;
    Vector2f min(const Vector2f& o) const;
    Vector2f max(const Vector2f& o) const;

    static constexpr Vector2f zero() { return Vector2f(0, 0); }
    static constexpr Vector2f one() { return Vector2f(1, 1); }

    [[nodiscard]] constexpr bool operator==(const Vector2f& o) const = default;
};

inline constexpr Vector2f operator*(float s, const Vector2f& v) { return v * s; }

// ---------------------------------------------------------------------------
// Vector3i
// ---------------------------------------------------------------------------
struct Vector3i {
    int x = 0;
    int y = 0;
    int z = 0;

    constexpr Vector3i() = default;
    constexpr Vector3i(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}

    [[nodiscard]] constexpr bool operator==(const Vector3i& o) const = default;
};

// ---------------------------------------------------------------------------
// Vector3f
// ---------------------------------------------------------------------------
struct Vector3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vector3f() = default;
    constexpr Vector3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vector3f operator+(const Vector3f& o) const { return Vector3f(x + o.x, y + o.y, z + o.z); }
    constexpr Vector3f operator-(const Vector3f& o) const { return Vector3f(x - o.x, y - o.y, z - o.z); }
    constexpr Vector3f operator*(float s) const { return Vector3f(x * s, y * s, z * s); }
    constexpr Vector3f operator/(float s) const { return Vector3f(x / s, y / s, z / s); }

    constexpr Vector3f& operator+=(const Vector3f& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vector3f& operator-=(const Vector3f& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vector3f& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr Vector3f& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }

    constexpr float dot(const Vector3f& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vector3f cross(const Vector3f& o) const {
        return Vector3f(
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        );
    }
    constexpr float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    Vector3f normalized() const { return *this / length(); }

    Vector3f lerp(const Vector3f& o, float t) const;
    Vector3f clamp(float min_val, float max_val) const;
    Vector3f clamp(const Vector3f& min_v, const Vector3f& max_v) const;
    Vector3f abs() const;
    Vector3f min(const Vector3f& o) const;
    Vector3f max(const Vector3f& o) const;
    float distance(const Vector3f& o) const;
    float distance_sq(const Vector3f& o) const;

    static constexpr Vector3f zero() { return Vector3f(0, 0, 0); }
    static constexpr Vector3f one() { return Vector3f(1, 1, 1); }
    static constexpr Vector3f up() { return Vector3f(0, 1, 0); }
    static constexpr Vector3f right() { return Vector3f(1, 0, 0); }
    static constexpr Vector3f forward() { return Vector3f(0, 0, -1); }

    [[nodiscard]] constexpr bool operator==(const Vector3f& o) const = default;
};

inline constexpr Vector3f operator*(float s, const Vector3f& v) { return v * s; }

// ---------------------------------------------------------------------------
// Vector4f
// ---------------------------------------------------------------------------
struct Vector4f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Vector4f() = default;
    constexpr Vector4f(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    explicit constexpr Vector4f(const Vector3f& v, float w_ = 1.0f) : x(v.x), y(v.y), z(v.z), w(w_) {}

    constexpr Vector4f operator+(const Vector4f& o) const { return Vector4f(x + o.x, y + o.y, z + o.z, w + o.w); }
    constexpr Vector4f operator-(const Vector4f& o) const { return Vector4f(x - o.x, y - o.y, z - o.z, w - o.w); }
    constexpr Vector4f operator*(float s) const { return Vector4f(x * s, y * s, z * s, w * s); }
    constexpr Vector4f operator/(float s) const { return Vector4f(x / s, y / s, z / s, w / s); }

    constexpr float dot(const Vector4f& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
    constexpr float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    Vector4f normalized() const { return *this / length(); }

    Vector3f xyz() const { return Vector3f(x, y, z); }

    [[nodiscard]] constexpr bool operator==(const Vector4f& o) const = default;
};

inline constexpr Vector4f operator*(float s, const Vector4f& v) { return v * s; }

// 前向声明（Matrix4f::from_quaternion 需要）
struct Quaternionf;

// ---------------------------------------------------------------------------
// Matrix4f（列主序：m[col * 4 + row]）
// ---------------------------------------------------------------------------
struct Matrix4f {
    // 默认构造初始化为 identity（原实现 m 未初始化，为未定义值）。
    // 渲染类成员大量直接默认构造使用，identity 比全零/垃圾值更安全；
    // 聚合初始化（identity()/from_diagonal() 等）不受影响。
    float m[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    constexpr float& operator()(int row, int col) { return m[col * 4 + row]; }
    constexpr const float& operator()(int row, int col) const { return m[col * 4 + row]; }

    static constexpr Matrix4f identity() {
        return Matrix4f{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
    }

    static constexpr Matrix4f from_diagonal(float x, float y, float z, float w = 1.0f) {
        return Matrix4f{
            x,    0.0f, 0.0f, 0.0f,
            0.0f, y,    0.0f, 0.0f,
            0.0f, 0.0f, z,    0.0f,
            0.0f, 0.0f, 0.0f, w
        };
    }

    constexpr Matrix4f operator+(const Matrix4f& o) const {
        Matrix4f r;
        for (int i = 0; i < 16; ++i) r.m[i] = m[i] + o.m[i];
        return r;
    }

    constexpr Matrix4f operator*(const Matrix4f& o) const {
        Matrix4f r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += (*this)(row, k) * o(k, col);
                }
                r(row, col) = sum;
            }
        }
        return r;
    }

    constexpr Vector4f operator*(const Vector4f& v) const {
        return Vector4f(
            m[ 0]*v.x + m[ 4]*v.y + m[ 8]*v.z + m[12]*v.w,
            m[ 1]*v.x + m[ 5]*v.y + m[ 9]*v.z + m[13]*v.w,
            m[ 2]*v.x + m[ 6]*v.y + m[10]*v.z + m[14]*v.w,
            m[ 3]*v.x + m[ 7]*v.y + m[11]*v.z + m[15]*v.w
        );
    }

    Vector3f transform_point(const Vector3f& v) const {
        Vector4f r = (*this) * Vector4f(v, 1.0f);
        return Vector3f(r.x / r.w, r.y / r.w, r.z / r.w);
    }

    Vector3f transform_vector(const Vector3f& v) const {
        Vector4f r = (*this) * Vector4f(v, 0.0f);
        return Vector3f(r.x, r.y, r.z);
    }

    Matrix4f transpose() const;
    Matrix4f inverse() const;

    static Matrix4f translate(float x, float y, float z);
    static Matrix4f translate(const Vector3f& v);
    static Matrix4f scale(float x, float y, float z);
    static Matrix4f scale(const Vector3f& v);
    static Matrix4f rotate(float angle, const Vector3f& axis);
    static Matrix4f from_quaternion(const Quaternionf& q);
    static Matrix4f look_at(const Vector3f& eye, const Vector3f& center, const Vector3f& up);
    static Matrix4f perspective(float fov, float aspect, float near, float far);
    static Matrix4f ortho(float left, float right, float bottom, float top, float near, float far);

    Vector3f right_vector() const;
    Vector3f up_vector() const;
    Vector3f forward_vector() const;
    Vector3f translation() const;

    [[nodiscard]] constexpr bool operator==(const Matrix4f& o) const = default;
};

// ---------------------------------------------------------------------------
// Quaternionf
// ---------------------------------------------------------------------------
struct Quaternionf {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quaternionf() = default;
    constexpr Quaternionf(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr Quaternionf identity() { return Quaternionf(0.0f, 0.0f, 0.0f, 1.0f); }

    static Quaternionf from_axis_angle(const Vector3f& axis, float angle);
    static Quaternionf from_euler(float pitch, float yaw, float roll);
    // 返回 (pitch, yaw, roll) 欧拉角（度），与 from_euler 同序。
    Vector3f to_euler() const;

    // 球面线性插值（slerp）：dot<0 时翻转取最短路径；
    // 两四元数几乎重合时退化为归一化线性插值（nlerp），避免 sinθ→0 除零。
    static Quaternionf slerp(const Quaternionf& a, const Quaternionf& b, float t);

    constexpr Quaternionf operator*(const Quaternionf& o) const {
        return Quaternionf(
            w*o.x + x*o.w + y*o.z - z*o.y,
            w*o.y - x*o.z + y*o.w + z*o.x,
            w*o.z + x*o.y - y*o.x + z*o.w,
            w*o.w - x*o.x - y*o.y - z*o.z
        );
    }

    constexpr Quaternionf conjugate() const { return Quaternionf(-x, -y, -z, w); }

    float length_sq() const { return x*x + y*y + z*z + w*w; }
    float length() const { return std::sqrt(length_sq()); }
    Quaternionf normalized() const { return *this / length(); }

    constexpr Quaternionf operator/(float s) const { return Quaternionf(x / s, y / s, z / s, w / s); }

    [[nodiscard]] constexpr bool operator==(const Quaternionf& o) const = default;

    Matrix4f to_matrix() const;
    Vector3f rotate_vector(const Vector3f& v) const;

    // 从纯旋转矩阵（3x3 基无缩放）构造四元数。
    // 用于 gizmo 等场景的矩阵 → TRS 分解；带缩放的矩阵需先归一化基向量。
    static Quaternionf from_rotation_matrix(const Matrix4f& m);
};

inline Matrix4f Matrix4f::from_quaternion(const Quaternionf& q) { return q.to_matrix(); }

} // namespace gryce_engine::math
