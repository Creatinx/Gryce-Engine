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
    // 标准快速 4x4 求逆（将矩阵按 2x2 子块分解，等价于伴随矩阵 / 行列式）。
    // 原实现用 4x4 余子式展开（det3 lambda 被调用 16 次）开销更大；
    // 下方用固定 12 个 2x2 行列式（b00..b11）展开，常数时间、无分支、缓存友好，
    // 与原实现对一般可逆矩阵逐位等价。
    const float a00 = (*this)(0, 0), a01 = (*this)(0, 1), a02 = (*this)(0, 2), a03 = (*this)(0, 3);
    const float a10 = (*this)(1, 0), a11 = (*this)(1, 1), a12 = (*this)(1, 2), a13 = (*this)(1, 3);
    const float a20 = (*this)(2, 0), a21 = (*this)(2, 1), a22 = (*this)(2, 2), a23 = (*this)(2, 3);
    const float a30 = (*this)(3, 0), a31 = (*this)(3, 1), a32 = (*this)(3, 2), a33 = (*this)(3, 3);

    const float b00 = a00 * a11 - a01 * a10;
    const float b01 = a00 * a12 - a02 * a10;
    const float b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11;
    const float b04 = a01 * a13 - a03 * a11;
    const float b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30;
    const float b07 = a20 * a32 - a22 * a30;
    const float b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31;
    const float b10 = a21 * a33 - a23 * a31;
    const float b11 = a22 * a33 - a23 * a32;

    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (std::abs(det) < 1e-6f) {
        return identity();
    }
    const float inv_det = 1.0f / det;

    Matrix4f inv;
    inv(0, 0) = ( a11 * b11 - a12 * b10 + a13 * b09) * inv_det;
    inv(0, 1) = (-a01 * b11 + a02 * b10 - a03 * b09) * inv_det;
    inv(0, 2) = ( a31 * b05 - a32 * b04 + a33 * b03) * inv_det;
    inv(0, 3) = (-a21 * b05 + a22 * b04 - a23 * b03) * inv_det;
    inv(1, 0) = (-a10 * b11 + a12 * b08 - a13 * b07) * inv_det;
    inv(1, 1) = ( a00 * b11 - a02 * b08 + a03 * b07) * inv_det;
    inv(1, 2) = (-a30 * b05 + a32 * b02 - a33 * b01) * inv_det;
    inv(1, 3) = ( a20 * b05 - a22 * b02 + a23 * b01) * inv_det;
    inv(2, 0) = ( a10 * b10 - a11 * b08 + a13 * b06) * inv_det;
    inv(2, 1) = (-a00 * b10 + a01 * b08 - a03 * b06) * inv_det;
    inv(2, 2) = ( a30 * b04 - a31 * b02 + a33 * b00) * inv_det;
    inv(2, 3) = (-a20 * b04 + a21 * b02 - a23 * b00) * inv_det;
    inv(3, 0) = (-a10 * b09 + a11 * b07 - a12 * b06) * inv_det;
    inv(3, 1) = ( a00 * b09 - a01 * b07 + a02 * b06) * inv_det;
    inv(3, 2) = (-a30 * b03 + a31 * b01 - a32 * b00) * inv_det;
    inv(3, 3) = ( a20 * b03 - a21 * b01 + a22 * b00) * inv_det;
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

Quaternionf Quaternionf::slerp(const Quaternionf& a, const Quaternionf& b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

    // dot<0：翻转 b 取最短路径（q 与 -q 表示同一旋转，插值应走短弧）
    Quaternionf b2 = b;
    if (dot < 0.0f) {
        dot = -dot;
        b2 = Quaternionf(-b.x, -b.y, -b.z, -b.w);
    }

    // 几乎重合：sinθ→0，slerp 公式不稳定，退化为 nlerp
    if (dot > 0.9995f) {
        return Quaternionf(
            a.x + (b2.x - a.x) * t,
            a.y + (b2.y - a.y) * t,
            a.z + (b2.z - a.z) * t,
            a.w + (b2.w - a.w) * t
        ).normalized();
    }

    const float theta = std::acos(dot);
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    return Quaternionf(
        a.x * wa + b2.x * wb,
        a.y * wa + b2.y * wb,
        a.z * wa + b2.z * wb,
        a.w * wa + b2.w * wb
    );
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

Vector3f Quaternionf::to_euler() const {
    // 与 from_euler 严格互逆。from_euler 合成的旋转为
    //     R = Rz(yaw) * Ry(pitch) * Rx(roll)
    // 即 pitch 绕 Y 轴、yaw 绕 Z 轴、roll 绕 X 轴（返回值与入参同序，单位：度）。
    // 展开后： m(2,0) = -sin(pitch)
    //          m(1,0) =  sin(yaw)cos(pitch),  m(0,0) = cos(yaw)cos(pitch)
    //          m(2,1) =  cos(pitch)sin(roll), m(2,2) = cos(pitch)cos(roll)
    Matrix4f m = to_matrix();
    const float m00 = m(0, 0), m01 = m(0, 1);
    const float m10 = m(1, 0), m11 = m(1, 1);
    const float m20 = m(2, 0), m21 = m(2, 1), m22 = m(2, 2);

    float pitch, yaw, roll;
    if (std::abs(m20) > 0.999999f) {
        // 万向节锁：pitch = ±90°，此时 yaw 与 roll 不再独立，约定 roll = 0 解出 yaw
        // （cp = 0 且 sr = 0 时 m01 = -sin(yaw)、m11 = cos(yaw)）
        pitch = (m20 < 0.0f) ? math::to_radians(90.0f) : math::to_radians(-90.0f);
        yaw = std::atan2(-m01, m11);
        roll = 0.0f;
    } else {
        pitch = std::asin(std::clamp(-m20, -1.0f, 1.0f));
        yaw = std::atan2(m10, m00);
        roll = std::atan2(m21, m22);
    }

    return Vector3f(math::to_degrees(pitch), math::to_degrees(yaw), math::to_degrees(roll));
}

Matrix4f Quaternionf::to_matrix() const {
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    // Matrix4f 是列主序（operator()(row, col) -> m[col * 4 + row]），而聚合初始化
    // 按内存序填充 m[0..15]，即 [第 0 列的 4 行][第 1 列的 4 行]...。
    // 所以这里必须按「旋转矩阵的列」书写，得到标准旋转矩阵 R，
    // 使 M * v 与 rotate_vector(v)、Matrix4f::rotate(angle, axis) 完全一致。
    // 历史版本按行书写，实际得到的是 Rᵀ（四元数的逆旋转）：凡由四元数构造的
    // 矩阵（实体 Transform、骨骼绑定矩阵、动画关键帧）都会把物体转到相反方向，
    // to_euler 与 from_euler 也因此无法往返。这里保留修正后的标准布局。
    return Matrix4f{
        1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz),         2.0f * (xz - wy),         0.0f,
        2.0f * (xy - wz),         1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),         0.0f,
        2.0f * (xz + wy),         2.0f * (yz - wx),         1.0f - 2.0f * (xx + yy), 0.0f,
        0.0f,                     0.0f,                     0.0f,                     1.0f
    };
}

Vector3f Quaternionf::rotate_vector(const Vector3f& v) const {
    // 优化公式：v' = v + 2w(q×v) + 2(q×(q×v))，
    // 避免一次额外的四元数乘法与共轭。对单位四元数与原 (q·vq·q⁻¹) 数学等价，
    // 且对接近退化的姿态数值更稳定。非单位四元数需先 normalize。
    const Vector3f qv(x, y, z);
    const Vector3f t = qv.cross(v) * 2.0f;
    return v + t * w + qv.cross(t);
}

Quaternionf Quaternionf::from_rotation_matrix(const Matrix4f& m) {
    // Shepperd 法：按最大对角元分四种路径，数值稳定。
    // 前提：m 为标准列主序旋转矩阵（与 to_matrix() / Matrix4f::rotate 同一约定），
    // 即 M*v 等价于 q 作用在 v 上，故 from_rotation_matrix(q.to_matrix()) ≈ q。
    const float m00 = m(0, 0), m01 = m(0, 1), m02 = m(0, 2);
    const float m10 = m(1, 0), m11 = m(1, 1), m12 = m(1, 2);
    const float m20 = m(2, 0), m21 = m(2, 1), m22 = m(2, 2);

    const float trace = m00 + m11 + m22;
    Quaternionf q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f; // s = 4x
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f; // s = 4y
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f; // s = 4z
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q.normalized();
}

} // namespace gryce_engine::math
