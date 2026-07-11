#include "math/math.h"

#include <cmath>

namespace gryce_engine::math {

// =========================================================================
// Vector2f
// =========================================================================
Vector2f Vector2f::lerp(const Vector2f& o, float t) const {
    return Vector2f(math::lerp(x, o.x, t), math::lerp(y, o.y, t));
}

Vector2f Vector2f::clamp(float min_val, float max_val) const {
    return Vector2f(
        math::clamp(x, min_val, max_val),
        math::clamp(y, min_val, max_val)
    );
}

Vector2f Vector2f::abs() const {
    return Vector2f(std::abs(x), std::abs(y));
}

Vector2f Vector2f::min(const Vector2f& o) const {
    return Vector2f(std::min(x, o.x), std::min(y, o.y));
}

Vector2f Vector2f::max(const Vector2f& o) const {
    return Vector2f(std::max(x, o.x), std::max(y, o.y));
}

// =========================================================================
// Vector3f
// =========================================================================
Vector3f Vector3f::lerp(const Vector3f& o, float t) const {
    return Vector3f(
        math::lerp(x, o.x, t),
        math::lerp(y, o.y, t),
        math::lerp(z, o.z, t)
    );
}

Vector3f Vector3f::clamp(float min_val, float max_val) const {
    return Vector3f(
        math::clamp(x, min_val, max_val),
        math::clamp(y, min_val, max_val),
        math::clamp(z, min_val, max_val)
    );
}

Vector3f Vector3f::clamp(const Vector3f& min_v, const Vector3f& max_v) const {
    return Vector3f(
        math::clamp(x, min_v.x, max_v.x),
        math::clamp(y, min_v.y, max_v.y),
        math::clamp(z, min_v.z, max_v.z)
    );
}

Vector3f Vector3f::abs() const {
    return Vector3f(std::abs(x), std::abs(y), std::abs(z));
}

Vector3f Vector3f::min(const Vector3f& o) const {
    return Vector3f(std::min(x, o.x), std::min(y, o.y), std::min(z, o.z));
}

Vector3f Vector3f::max(const Vector3f& o) const {
    return Vector3f(std::max(x, o.x), std::max(y, o.y), std::max(z, o.z));
}

float Vector3f::distance(const Vector3f& o) const {
    return (*this - o).length();
}

float Vector3f::distance_sq(const Vector3f& o) const {
    return (*this - o).length_sq();
}

// =========================================================================
// Matrix4f
// =========================================================================
Matrix4f Matrix4f::transpose() const {
    Matrix4f r;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r(row, col) = (*this)(col, row);
        }
    }
    return r;
}

Matrix4f Matrix4f::inverse() const {
    // 3x3 子矩阵行列式（通过 operator() 访问，避免列主序索引混乱）
    auto det3 = [this](int r0, int c0, int r1, int c1, int r2, int c2) -> float {
        float a11 = (*this)(r0, c0), a12 = (*this)(r0, c1), a13 = (*this)(r0, c2);
        float a21 = (*this)(r1, c0), a22 = (*this)(r1, c1), a23 = (*this)(r1, c2);
        float a31 = (*this)(r2, c0), a32 = (*this)(r2, c1), a33 = (*this)(r2, c2);
        return a11*(a22*a33 - a23*a32)
             - a12*(a21*a33 - a23*a31)
             + a13*(a21*a32 - a22*a31);
    };

    // 去掉第 i 行后剩余的行索引
    const int rows[4][3] = {
        {1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}
    };
    // 去掉第 j 列后剩余的列索引
    const int cols[4][3] = {
        {1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}
    };

    float cof[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sign = ((r + c) % 2 == 0) ? 1.0f : -1.0f;
            cof[r][c] = sign * det3(rows[r][0], cols[c][0], rows[r][1], cols[c][1], rows[r][2], cols[c][2]);
        }
    }

    // 按第 0 列展开求行列式
    float det = (*this)(0,0)*cof[0][0] + (*this)(1,0)*cof[1][0] + (*this)(2,0)*cof[2][0] + (*this)(3,0)*cof[3][0];
    if (std::abs(det) < 1e-6f) {
        return identity();
    }
    det = 1.0f / det;

    // 伴随矩阵 = 余子式矩阵的转置
    Matrix4f inv;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            inv(r, c) = cof[c][r] * det;
        }
    }
    return inv;
}

Matrix4f Matrix4f::translate(float x, float y, float z) {
    Matrix4f r = identity();
    r(0, 3) = x;
    r(1, 3) = y;
    r(2, 3) = z;
    return r;
}

Matrix4f Matrix4f::translate(const Vector3f& v) {
    return translate(v.x, v.y, v.z);
}

Matrix4f Matrix4f::scale(float x, float y, float z) {
    return from_diagonal(x, y, z, 1.0f);
}

Matrix4f Matrix4f::scale(const Vector3f& v) {
    return from_diagonal(v.x, v.y, v.z, 1.0f);
}

Matrix4f Matrix4f::rotate(float angle, const Vector3f& axis) {
    Vector3f n = axis.normalized();
    float c = std::cos(angle);
    float s = std::sin(angle);
    float t = 1.0f - c;
    float x = n.x, y = n.y, z = n.z;

    Matrix4f r;
    r(0,0) = t*x*x + c;     r(0,1) = t*x*y - s*z;   r(0,2) = t*x*z + s*y;   r(0,3) = 0.0f;
    r(1,0) = t*x*y + s*z;   r(1,1) = t*y*y + c;     r(1,2) = t*y*z - s*x;   r(1,3) = 0.0f;
    r(2,0) = t*x*z - s*y;   r(2,1) = t*y*z + s*x;   r(2,2) = t*z*z + c;     r(2,3) = 0.0f;
    r(3,0) = 0.0f;          r(3,1) = 0.0f;          r(3,2) = 0.0f;          r(3,3) = 1.0f;
    return r;
}

Matrix4f Matrix4f::look_at(const Vector3f& eye, const Vector3f& center, const Vector3f& world_up) {
    Vector3f f = (center - eye).normalized();
    Vector3f s = f.cross(world_up).normalized();
    Vector3f u = s.cross(f);

    Matrix4f r = identity();
    r(0,0) = s.x;   r(0,1) = s.y;   r(0,2) = s.z;   r(0,3) = -s.dot(eye);
    r(1,0) = u.x;   r(1,1) = u.y;   r(1,2) = u.z;   r(1,3) = -u.dot(eye);
    r(2,0) = -f.x;  r(2,1) = -f.y;  r(2,2) = -f.z;  r(2,3) = f.dot(eye);
    return r;
}

Matrix4f Matrix4f::perspective(float fov, float aspect, float near, float far) {
    float tan_half_fov = std::tan(fov * 0.5f);
    Matrix4f r = identity();
    r(0,0) = 1.0f / (aspect * tan_half_fov);
    r(1,1) = 1.0f / tan_half_fov;
    r(2,2) = (far + near) / (near - far);
    r(2,3) = (2.0f * far * near) / (near - far);
    r(3,2) = -1.0f;
    r(3,3) = 0.0f;
    return r;
}

Matrix4f Matrix4f::ortho(float left, float right, float bottom, float top, float near, float far) {
    Matrix4f r = identity();
    r(0,0) = 2.0f / (right - left);
    r(1,1) = 2.0f / (top - bottom);
    r(2,2) = -2.0f / (far - near);
    r(0,3) = -(right + left) / (right - left);
    r(1,3) = -(top + bottom) / (top - bottom);
    r(2,3) = -(far + near) / (far - near);
    return r;
}

Vector3f Matrix4f::right_vector() const {
    return Vector3f((*this)(0,0), (*this)(1,0), (*this)(2,0)).normalized();
}

Vector3f Matrix4f::up_vector() const {
    return Vector3f((*this)(0,1), (*this)(1,1), (*this)(2,1)).normalized();
}

Vector3f Matrix4f::forward_vector() const {
    return Vector3f(-(*this)(0,2), -(*this)(1,2), -(*this)(2,2)).normalized();
}

Vector3f Matrix4f::translation() const {
    return Vector3f((*this)(0,3), (*this)(1,3), (*this)(2,3));
}

// =========================================================================
// Quaternionf
// =========================================================================
Quaternionf Quaternionf::from_axis_angle(const Vector3f& axis, float angle) {
    float half = angle * 0.5f;
    float s = std::sin(half);
    Vector3f n = axis.normalized();
    return Quaternionf(n.x * s, n.y * s, n.z * s, std::cos(half));
}

Quaternionf Quaternionf::from_euler(float pitch, float yaw, float roll) {
    float cr = std::cos(roll * 0.5f),  sr = std::sin(roll * 0.5f);
    float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    float cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);

    return Quaternionf(
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy
    );
}

Matrix4f Quaternionf::to_matrix() const {
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    return Matrix4f{
        1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz),         2.0f * (xz + wy),         0.0f,
        2.0f * (xy + wz),         1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx),         0.0f,
        2.0f * (xz - wy),         2.0f * (yz + wx),         1.0f - 2.0f * (xx + yy), 0.0f,
        0.0f,                     0.0f,                     0.0f,                     1.0f
    };
}

Vector3f Quaternionf::rotate_vector(const Vector3f& v) const {
    Quaternionf vq(v.x, v.y, v.z, 0.0f);
    Quaternionf r = (*this) * vq * conjugate();
    return Vector3f(r.x, r.y, r.z);
}

} // namespace gryce_engine::math
