#pragma once

#include <cmath>

#include "math/math.h"

namespace gryce_engine::math {

// ---------------------------------------------------------------------------
// Ray — 三维射线（origin + 单位 direction）
// 供编辑器点选拾取、物理探测等使用。
// ---------------------------------------------------------------------------
struct Ray {
    Vector3f origin;
    Vector3f direction = Vector3f(0.0f, 0.0f, -1.0f);

    // 射线上 t 处的点
    Vector3f at(float t) const { return origin + direction * t; }
};

// ---------------------------------------------------------------------------
// ray_intersect_aabb — slab 法射线/AABB 求交
// 命中时返回 true，out_t 为进入距离（起点在盒内时为 0 附近的退出侧仍取进入 t，
// 本实现返回 tmin，可能为负值表示起点已在盒内，调用方可按需 clamp）。
// ---------------------------------------------------------------------------
inline bool ray_intersect_aabb(const Ray& ray,
                               const Vector3f& bmin, const Vector3f& bmax,
                               float& out_t) {
    float tmin = -1e30f;
    float tmax = 1e30f;

    const float* origin = &ray.origin.x;
    const float* dir = &ray.direction.x;
    const float* lo = &bmin.x;
    const float* hi = &bmax.x;

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-8f) {
            // 射线与该轴平行：起点必须在 slab 内才可能有交
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return false;
            continue;
        }
        float t1 = (lo[axis] - origin[axis]) / dir[axis];
        float t2 = (hi[axis] - origin[axis]) / dir[axis];
        if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
        tmin = t1 > tmin ? t1 : tmin;
        tmax = t2 < tmax ? t2 : tmax;
        if (tmin > tmax) return false;
    }

    // 整个 AABB 在射线后方
    if (tmax < 0.0f) return false;

    out_t = tmin;
    return true;
}

// ---------------------------------------------------------------------------
// screen_ndc_to_ray — NDC 屏幕点（[-1,1]，y 向上）→ 世界空间射线
// inv_view_proj 为 (projection * view) 的逆矩阵。
// 取 NDC z=-1（近平面）与 z=+1（远平面）两点反投影确定射线方向，
// 与 GL 风格 perspective 矩阵约定一致。
// ---------------------------------------------------------------------------
inline Vector3f unproject_ndc(float x, float y, float z, const Matrix4f& inv_view_proj) {
    const Vector4f p = inv_view_proj * Vector4f(x, y, z, 1.0f);
    const float inv_w = (p.w != 0.0f) ? 1.0f / p.w : 1.0f;
    return Vector3f(p.x * inv_w, p.y * inv_w, p.z * inv_w);
}

inline Ray screen_ndc_to_ray(float ndc_x, float ndc_y, const Matrix4f& inv_view_proj) {
    const Vector3f p_near = unproject_ndc(ndc_x, ndc_y, -1.0f, inv_view_proj);
    const Vector3f p_far = unproject_ndc(ndc_x, ndc_y, 1.0f, inv_view_proj);
    Ray ray;
    ray.origin = p_near;
    ray.direction = (p_far - p_near).normalized();
    return ray;
}

} // namespace gryce_engine::math
