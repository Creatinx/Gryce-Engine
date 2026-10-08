// ===========================================================================
// test/math_tests.cpp
// 纯计算单元测试：只核对数值结果，不依赖 GPU / 窗口 / 截图 / 引擎 DLL。
//
// 被测源码直接编译进本可执行文件（engine/math/math.cpp + camera.cpp），
// 既不链接 GryceCore.dll，也不需要同步任何运行库 —— CI 上 ctest 可以在
// 毫秒级完成，且与驱动、显卡、无头环境完全无关（详见根 CMakeLists 注释）。
//
// 每个 suite 在 CMake 里注册成一条独立的 CTest：
//   ctest 任一条失败 -> 进程返回非零 -> CI 判定不通过。
// 用 --suite <name> 可单独跑某个 suite，--list 列出全部。
// ===========================================================================

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "math/camera.h"
#include "math/math.h"
#include "math/ray.h"

using gryce_engine::math::Camera;
using gryce_engine::math::Matrix4f;
using gryce_engine::math::Quaternionf;
using gryce_engine::math::Ray;
using gryce_engine::math::Vector2f;
using gryce_engine::math::Vector3f;
using gryce_engine::math::Vector4f;

namespace tc {
using gryce_engine::math::clamp;
using gryce_engine::math::lerp;
using gryce_engine::math::saturate;
using gryce_engine::math::to_degrees;
using gryce_engine::math::to_radians;
} // namespace tc

// ===========================================================================
// 迷你断言框架（无第三方依赖）
// ===========================================================================
namespace {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

// 单个用例内的统计
int g_checks = 0;
int g_checks_failed = 0;
int g_case_checks = 0;
int g_case_failed = 0;
std::string g_current;

void begin_case(const std::string& suite, const std::string& name) {
    g_current = suite + "." + name;
    g_case_checks = 0;
    g_case_failed = 0;
}

void fail_at(const char* file, int line, const std::string& msg) {
    ++g_case_failed;
    ++g_checks_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
    std::fflush(stdout);
}

constexpr float kEps = 1e-4f;      // 常规浮点比较容差
constexpr float kLooseEps = 1e-3f; // 矩阵求逆 / 反投影等有放大误差处

bool nearly(float a, float b, float eps) {
    const float diff = std::fabs(a - b);
    if (diff <= eps) return true;
    // 相对容差：处理坐标绝对值较大的情形（如 far=100 的反投影）
    const float scale = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return diff <= eps * (scale > 1.0f ? scale : 1.0f);
}

} // namespace

#define TEST(suite, name)                                                            \
    static void suite##__##name##_impl();                                             \
    static ::Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl);  \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                             \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                              \
        if (!(expr)) {                                                               \
            ::fail_at(__FILE__, __LINE__, "expected true: " #expr);                   \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                              \
        const float a_ = static_cast<float>(actual);                                  \
        const float b_ = static_cast<float>(expected);                                \
        if (!::nearly(a_, b_, (eps))) {                                               \
            char buf[320];                                                            \
            std::snprintf(buf, sizeof(buf), "%s => %.8g, expected %.8g (eps %g)",     \
                          #actual, static_cast<double>(a_), static_cast<double>(b_),  \
                          static_cast<double>(eps));                                  \
            ::fail_at(__FILE__, __LINE__, buf);                                       \
        }                                                                            \
    } while (0)

// 注意：宏形参不能叫 x/y/z/w，否则会把成员访问里的 .x 也替换掉。
#define CHECK_VEC2(vec_, ex_, ey_, eps_)        \
    do {                                        \
        CHECK_NEAR((vec_).x, (ex_), eps_);       \
        CHECK_NEAR((vec_).y, (ey_), eps_);       \
    } while (0)

#define CHECK_VEC3(vec_, ex_, ey_, ez_, eps_)   \
    do {                                        \
        CHECK_NEAR((vec_).x, (ex_), eps_);       \
        CHECK_NEAR((vec_).y, (ey_), eps_);       \
        CHECK_NEAR((vec_).z, (ez_), eps_);       \
    } while (0)

#define CHECK_VEC4(vec_, ex_, ey_, ez_, ew_, eps_) \
    do {                                        \
        CHECK_NEAR((vec_).x, (ex_), eps_);       \
        CHECK_NEAR((vec_).y, (ey_), eps_);       \
        CHECK_NEAR((vec_).z, (ez_), eps_);       \
        CHECK_NEAR((vec_).w, (ew_), eps_);       \
    } while (0)

#define CHECK_UNIT_VEC3(a, eps) CHECK_NEAR((a).length(), 1.0f, eps)

// ===========================================================================
// suite: vector —— 标量工具与 2/3/4 维向量
// ===========================================================================
TEST(vector, scalar_helpers) {
    CHECK_NEAR(tc::to_radians(180.0f), 3.14159265f, kEps);
    CHECK_NEAR(tc::to_radians(90.0f) * 2.0f, 3.14159265f, kEps);
    CHECK_NEAR(tc::to_degrees(3.14159265f), 180.0f, kEps);
    CHECK_NEAR(tc::to_degrees(tc::to_radians(37.0f)), 37.0f, kEps);

    CHECK_NEAR(tc::lerp(2.0f, 4.0f, 0.25f), 2.5f, kEps);
    CHECK_NEAR(tc::lerp(2.0f, 4.0f, 0.0f), 2.0f, kEps);
    CHECK_NEAR(tc::lerp(2.0f, 4.0f, 1.0f), 4.0f, kEps);

    CHECK_NEAR(tc::clamp(5.0f, 0.0f, 1.0f), 1.0f, kEps);
    CHECK_NEAR(tc::clamp(-3.0f, 0.0f, 1.0f), 0.0f, kEps);
    CHECK_NEAR(tc::clamp(0.42f, 0.0f, 1.0f), 0.42f, kEps);
    CHECK_NEAR(tc::saturate(2.0f), 1.0f, kEps);
    CHECK_NEAR(tc::saturate(-2.0f), 0.0f, kEps);
}

TEST(vector, vector2_basics) {
    const Vector2f a(3.0f, 4.0f);
    const Vector2f b(1.0f, -2.0f);

    CHECK_VEC2(a + b, 4.0f, 2.0f, kEps);
    CHECK_VEC2(a - b, 2.0f, 6.0f, kEps);
    CHECK_VEC2(a * 2.0f, 6.0f, 8.0f, kEps);
    CHECK_VEC2(2.0f * a, 6.0f, 8.0f, kEps);
    CHECK_VEC2(a / 2.0f, 1.5f, 2.0f, kEps);

    CHECK_NEAR(a.dot(b), 3.0f * 1.0f + 4.0f * -2.0f, kEps);
    CHECK_NEAR(a.length_sq(), 25.0f, kEps);
    CHECK_NEAR(a.length(), 5.0f, kEps);
    CHECK_VEC2(a.normalized(), 0.6f, 0.8f, kEps);
    CHECK_NEAR(a.normalized().length(), 1.0f, kEps);

    CHECK_VEC2(Vector2f(1.0f, 2.0f).lerp(Vector2f(3.0f, 4.0f), 0.5f), 2.0f, 3.0f, kEps);
    CHECK_VEC2(Vector2f(-3.0f, 9.0f).abs(), 3.0f, 9.0f, kEps);
    CHECK_VEC2(Vector2f(1.0f, 5.0f).min(Vector2f(3.0f, 2.0f)), 1.0f, 2.0f, kEps);
    CHECK_VEC2(Vector2f(1.0f, 5.0f).max(Vector2f(3.0f, 2.0f)), 3.0f, 5.0f, kEps);
    CHECK_VEC2(Vector2f(1.5f, -0.5f).clamp(0.0f, 1.0f), 1.0f, 0.0f, kEps);

    CHECK_TRUE(Vector2f::zero() == Vector2f(0.0f, 0.0f));
    CHECK_TRUE(Vector2f::one() == Vector2f(1.0f, 1.0f));

    Vector2f acc(1.0f, 1.0f);
    acc += Vector2f(1.0f, 2.0f);
    CHECK_VEC2(acc, 2.0f, 3.0f, kEps);
    acc -= Vector2f(1.0f, 2.0f);
    CHECK_VEC2(acc, 1.0f, 1.0f, kEps);
    acc *= 3.0f;
    CHECK_VEC2(acc, 3.0f, 3.0f, kEps);
    acc /= 3.0f;
    CHECK_VEC2(acc, 1.0f, 1.0f, kEps);
}

TEST(vector, vector3_basics) {
    const Vector3f a(1.0f, 2.0f, 3.0f);
    const Vector3f b(4.0f, -5.0f, 6.0f);

    CHECK_VEC3(a + b, 5.0f, -3.0f, 9.0f, kEps);
    CHECK_VEC3(a - b, -3.0f, 7.0f, -3.0f, kEps);
    CHECK_VEC3(a * 2.0f, 2.0f, 4.0f, 6.0f, kEps);
    CHECK_VEC3(2.0f * a, 2.0f, 4.0f, 6.0f, kEps);
    CHECK_VEC3(a / 2.0f, 0.5f, 1.0f, 1.5f, kEps);

    CHECK_NEAR(a.dot(b), 4.0f - 10.0f + 18.0f, kEps);
    CHECK_NEAR(Vector3f::right().dot(Vector3f::up()), 0.0f, kEps);

    // 右手系：X × Y = Z
    CHECK_VEC3(Vector3f::right().cross(Vector3f::up()), 0.0f, 0.0f, 1.0f, kEps);
    // Y × Z = X
    CHECK_VEC3(Vector3f::up().cross(Vector3f(0.0f, 0.0f, 1.0f)), 1.0f, 0.0f, 0.0f, kEps);
    // 反交换律
    const Vector3f c1 = a.cross(b);
    const Vector3f c2 = b.cross(a);
    CHECK_NEAR(c1.x, -c2.x, kEps);
    CHECK_NEAR(c1.y, -c2.y, kEps);
    CHECK_NEAR(c1.z, -c2.z, kEps);
    // 与自身叉乘为零
    CHECK_NEAR(a.cross(a).length_sq(), 0.0f, kEps);
    // 叉乘结果同时垂直于两个操作数
    CHECK_NEAR(c1.dot(a), 0.0f, kEps);
    CHECK_NEAR(c1.dot(b), 0.0f, kEps);

    const Vector3f v(3.0f, 4.0f, 0.0f);
    CHECK_NEAR(v.length_sq(), 25.0f, kEps);
    CHECK_NEAR(v.length(), 5.0f, kEps);
    CHECK_VEC3(v.normalized(), 0.6f, 0.8f, 0.0f, kEps);

    CHECK_NEAR(Vector3f::zero().distance(v), 5.0f, kEps);
    CHECK_NEAR(Vector3f::zero().distance_sq(v), 25.0f, kEps);

    CHECK_VEC3(Vector3f(0.0f, 0.0f, 0.0f).lerp(Vector3f(10.0f, 10.0f, 10.0f), 0.3f),
               3.0f, 3.0f, 3.0f, kEps);
    CHECK_VEC3(Vector3f(-1.0f, 2.0f, -3.0f).abs(), 1.0f, 2.0f, 3.0f, kEps);
    CHECK_VEC3(Vector3f(1.0f, 5.0f, 2.0f).min(Vector3f(3.0f, 2.0f, 4.0f)),
               1.0f, 2.0f, 2.0f, kEps);
    CHECK_VEC3(Vector3f(1.0f, 5.0f, 2.0f).max(Vector3f(3.0f, 2.0f, 4.0f)),
               3.0f, 5.0f, 4.0f, kEps);
    CHECK_VEC3(Vector3f(0.5f, 2.0f, -2.0f).clamp(0.0f, 1.0f), 0.5f, 1.0f, 0.0f, kEps);
    CHECK_VEC3(Vector3f(0.5f, 2.0f, -2.0f).clamp(Vector3f(0.0f, 0.0f, -1.0f), Vector3f(1.0f, 1.0f, 1.0f)),
               0.5f, 1.0f, -1.0f, kEps);

    CHECK_TRUE(Vector3f::forward() == Vector3f(0.0f, 0.0f, -1.0f));
}

TEST(vector, vector4_basics) {
    const Vector4f a(1.0f, 2.0f, 3.0f, 4.0f);
    const Vector4f b(1.0f, 1.0f, 1.0f, 1.0f);

    CHECK_VEC4(a + b, 2.0f, 3.0f, 4.0f, 5.0f, kEps);
    CHECK_VEC4(a - b, 0.0f, 1.0f, 2.0f, 3.0f, kEps);
    CHECK_VEC4(a * 2.0f, 2.0f, 4.0f, 6.0f, 8.0f, kEps);
    CHECK_VEC4(a / 2.0f, 0.5f, 1.0f, 1.5f, 2.0f, kEps);
    CHECK_NEAR(a.dot(b), 10.0f, kEps);
    CHECK_NEAR(a.length_sq(), 1.0f + 4.0f + 9.0f + 16.0f, kEps);
    CHECK_NEAR(a.length(), 5.4772256f, kEps);
    CHECK_VEC3(a.xyz(), 1.0f, 2.0f, 3.0f, kEps);

    // 由 Vector3f 构造，w 默认 1（点）
    const Vector4f p(Vector3f(1.0f, 2.0f, 3.0f));
    CHECK_VEC4(p, 1.0f, 2.0f, 3.0f, 1.0f, kEps);
    const Vector4f d(Vector3f(1.0f, 2.0f, 3.0f), 0.0f);
    CHECK_NEAR(d.w, 0.0f, kEps);
}

// ===========================================================================
// suite: matrix —— 构造、乘法约定、旋转、求逆、投影矩阵
// ===========================================================================
TEST(matrix, identity_and_basis) {
    const Matrix4f I = Matrix4f::identity();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(I(r, c), (r == c) ? 1.0f : 0.0f, kEps);
        }
    }
    // 默认构造即单位矩阵（渲染层大量默认构造依赖这一点）
    Matrix4f def;
    CHECK_NEAR(def(0, 0), 1.0f, kEps);
    CHECK_NEAR(def(1, 1), 1.0f, kEps);
    CHECK_NEAR(def(3, 3), 1.0f, kEps);
    CHECK_TRUE(def == I);

    const Vector4f v(1.0f, 2.0f, 3.0f, 1.0f);
    CHECK_VEC4(I * v, 1.0f, 2.0f, 3.0f, 1.0f, kEps);
    CHECK_TRUE((I * I) == I);
}

TEST(matrix, translate_scale) {
    const Matrix4f T = Matrix4f::translate(10.0f, 20.0f, 30.0f);
    CHECK_VEC3(T.translation(), 10.0f, 20.0f, 30.0f, kEps);
    // 点受平移影响
    CHECK_VEC3(T.transform_point(Vector3f(1.0f, 2.0f, 3.0f)), 11.0f, 22.0f, 33.0f, kEps);
    // 方向不受平移影响（w = 0）
    CHECK_VEC3(T.transform_vector(Vector3f(1.0f, 2.0f, 3.0f)), 1.0f, 2.0f, 3.0f, kEps);

    const Matrix4f S = Matrix4f::scale(2.0f, 3.0f, 4.0f);
    CHECK_VEC3(S.transform_vector(Vector3f(1.0f, 1.0f, 1.0f)), 2.0f, 3.0f, 4.0f, kEps);
    CHECK_VEC3(S.transform_point(Vector3f(1.0f, 1.0f, 1.0f)), 2.0f, 3.0f, 4.0f, kEps);

    // Vector3f 重载
    CHECK_VEC3(Matrix4f::translate(Vector3f(1.0f, 2.0f, 3.0f)).translation(), 1.0f, 2.0f, 3.0f, kEps);
    CHECK_VEC3(Matrix4f::scale(Vector3f(2.0f, 2.0f, 2.0f)).transform_vector(Vector3f(1.0f, 2.0f, 3.0f)),
               2.0f, 4.0f, 6.0f, kEps);
}

TEST(matrix, multiply_order) {
    const Matrix4f T = Matrix4f::translate(100.0f, 0.0f, 0.0f);
    const Matrix4f S = Matrix4f::scale(2.0f, 2.0f, 2.0f);

    // 约定：M = T * S 表示先缩放再平移（右乘先作用于向量）
    CHECK_VEC3((T * S).transform_point(Vector3f(1.0f, 1.0f, 1.0f)), 102.0f, 2.0f, 2.0f, kEps);
    // 反过来 S * T 是先平移再缩放，平移量也被放大
    CHECK_VEC3((S * T).transform_point(Vector3f(1.0f, 1.0f, 1.0f)), 202.0f, 2.0f, 2.0f, kEps);

    // 乘法结合律
    const Matrix4f R = Matrix4f::rotate(tc::to_radians(30.0f), Vector3f(0.0f, 1.0f, 0.0f));
    const Vector3f p(1.0f, 2.0f, 3.0f);
    const Vector3f lhs = ((T * S) * R).transform_point(p);
    const Vector3f rhs = (T * (S * R)).transform_point(p);
    CHECK_VEC3(lhs, rhs.x, rhs.y, rhs.z, kEps);

    // 单位元
    CHECK_VEC3((Matrix4f::identity() * T).transform_point(p), 101.0f, 2.0f, 3.0f, kEps);
}

TEST(matrix, rotation_axes) {
    const float k90 = tc::to_radians(90.0f);
    const Vector3f x(1.0f, 0.0f, 0.0f);
    const Vector3f y(0.0f, 1.0f, 0.0f);
    const Vector3f z(0.0f, 0.0f, 1.0f);

    // 绕 +Z 转 90°：X -> Y
    CHECK_VEC3(Matrix4f::rotate(k90, z).transform_vector(x), 0.0f, 1.0f, 0.0f, kEps);
    // 绕 +Y 转 90°：Z -> X
    CHECK_VEC3(Matrix4f::rotate(k90, y).transform_vector(z), 1.0f, 0.0f, 0.0f, kEps);
    // 绕 +X 转 90°：Y -> Z
    CHECK_VEC3(Matrix4f::rotate(k90, x).transform_vector(y), 0.0f, 0.0f, 1.0f, kEps);

    // 180°：反向
    CHECK_VEC3(Matrix4f::rotate(tc::to_radians(180.0f), z).transform_vector(x), -1.0f, 0.0f, 0.0f, kEps);

    // 旋转轴上的向量不变
    CHECK_VEC3(Matrix4f::rotate(k90, y).transform_vector(y), 0.0f, 1.0f, 0.0f, kEps);

    // 旋转保长度、保手性（三个基仍两两正交）
    const Matrix4f R = Matrix4f::rotate(tc::to_radians(37.0f), Vector3f(1.0f, 2.0f, 3.0f));
    const Vector3f rx = R.transform_vector(x);
    const Vector3f ry = R.transform_vector(y);
    const Vector3f rz = R.transform_vector(z);
    CHECK_NEAR(rx.length(), 1.0f, kEps);
    CHECK_NEAR(ry.length(), 1.0f, kEps);
    CHECK_NEAR(rz.length(), 1.0f, kEps);
    CHECK_NEAR(rx.dot(ry), 0.0f, kEps);
    CHECK_NEAR(ry.dot(rz), 0.0f, kEps);
    CHECK_NEAR(rz.cross(rx).dot(ry), 1.0f, kEps);
    // 右手系：(rx × ry) 应与 rz 同向
    CHECK_NEAR(rx.cross(ry).dot(rz), 1.0f, kEps);
    CHECK_NEAR(ry.cross(rz).dot(rx), 1.0f, kEps);
    CHECK_NEAR(rz.cross(rx).dot(ry), 1.0f, kEps);

    // 逆旋转 = 反向同角度
    const Matrix4f Rinv = Matrix4f::rotate(-tc::to_radians(37.0f), Vector3f(1.0f, 2.0f, 3.0f));
    CHECK_VEC3(Rinv.transform_vector(rx), 1.0f, 0.0f, 0.0f, kEps);
}

TEST(matrix, transpose) {
    Matrix4f m = Matrix4f::identity();
    for (int i = 0; i < 16; ++i) {
        m.m[i] = static_cast<float>(i + 1);
    }
    const Matrix4f t = m.transpose();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(t(r, c), m(c, r), kEps);
        }
    }
    // 转置是对合运算
    for (int i = 0; i < 16; ++i) {
        CHECK_NEAR(t.transpose().m[i], m.m[i], kEps);
    }
    // 单位矩阵的转置是自身
    CHECK_TRUE(Matrix4f::identity().transpose() == Matrix4f::identity());
}

TEST(matrix, inverse) {
    // 平移的逆是反向平移
    const Matrix4f T = Matrix4f::translate(4.0f, -5.0f, 6.0f);
    CHECK_VEC3(T.inverse().translation(), -4.0f, 5.0f, -6.0f, kEps);

    // 缩放的逆是倒数缩放
    const Vector3f sv = Matrix4f::scale(2.0f, 4.0f, 8.0f).inverse().transform_vector(Vector3f(2.0f, 4.0f, 8.0f));
    CHECK_VEC3(sv, 1.0f, 1.0f, 1.0f, kEps);

    // 复合矩阵：M * M⁻¹ = I 且 M⁻¹ * M = I
    const Matrix4f M = Matrix4f::translate(3.0f, -2.0f, 7.0f)
                     * Matrix4f::rotate(tc::to_radians(48.0f), Vector3f(0.3f, -1.2f, 0.7f))
                     * Matrix4f::scale(1.5f, 2.5f, 0.5f);
    const Matrix4f inv = M.inverse();
    Matrix4f should_be_i = M * inv;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(should_be_i(r, c), (r == c) ? 1.0f : 0.0f, kLooseEps);
        }
    }
    should_be_i = inv * M;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(should_be_i(r, c), (r == c) ? 1.0f : 0.0f, kLooseEps);
        }
    }

    // 往返：先变过去再变回来应还原
    const Vector3f p(9.0f, -4.0f, 2.0f);
    CHECK_VEC3(inv.transform_point(M.transform_point(p)), p.x, p.y, p.z, kLooseEps);

    // 奇异矩阵（行列式为 0）退化为单位矩阵，不应产生 NaN
    const Matrix4f singular = Matrix4f::from_diagonal(1.0f, 1.0f, 0.0f, 1.0f);
    const Matrix4f si = singular.inverse();
    CHECK_TRUE(si == Matrix4f::identity());
}

TEST(matrix, perspective) {
    const float fov = tc::to_radians(60.0f);
    const float aspect = 16.0f / 9.0f;
    const float near = 0.1f;
    const float far = 100.0f;
    const Matrix4f P = Matrix4f::perspective(fov, aspect, near, far);

    // 近平面 -> NDC z = -1；远平面 -> NDC z = +1
    const Vector4f cn = P * Vector4f(0.0f, 0.0f, -near, 1.0f);
    CHECK_NEAR(cn.z / cn.w, -1.0f, kEps);
    const Vector4f cf = P * Vector4f(0.0f, 0.0f, -far, 1.0f);
    CHECK_NEAR(cf.z / cf.w, 1.0f, kEps);

    // 中平面（near 与 far 的几何中点）落在 z=0 之后（透视非线性），只验 monotonic
    const Vector4f cm = P * Vector4f(0.0f, 0.0f, -(near + far) * 0.5f, 1.0f);
    CHECK_TRUE(cm.z / cm.w > -1.0f && cm.z / cm.w < 1.0f);

    // 光轴上任意点投影到 NDC 中心
    const Vector4f axis = P * Vector4f(0.0f, 0.0f, -12.0f, 1.0f);
    CHECK_NEAR(axis.x / axis.w, 0.0f, kEps);
    CHECK_NEAR(axis.y / axis.w, 0.0f, kEps);
    CHECK_NEAR(axis.w, 12.0f, kLooseEps); // w = 到相机的距离

    // 视线边界：y = tan(fov/2) * d 处正好映射到 NDC y = +1
    const float tan_half = std::tan(fov * 0.5f);
    const Vector4f top = P * Vector4f(0.0f, tan_half * 5.0f, -5.0f, 1.0f);
    CHECK_NEAR(top.y / top.w, 1.0f, kEps);
    // x 方向要除以 aspect
    const Vector4f right_edge = P * Vector4f(tan_half * aspect * 5.0f, 0.0f, -5.0f, 1.0f);
    CHECK_NEAR(right_edge.x / right_edge.w, 1.0f, kEps);

    // 相机后方的点 w < 0（会被裁剪）
    CHECK_TRUE((P * Vector4f(0.0f, 0.0f, 5.0f, 1.0f)).w < 0.0f);
}

TEST(matrix, ortho) {
    const Matrix4f O = Matrix4f::ortho(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 11.0f);

    // 六个面都映射到 NDC 边界
    CHECK_NEAR((O * Vector4f(2.0f, 0.0f, -1.0f, 1.0f)).x, 1.0f, kEps);
    CHECK_NEAR((O * Vector4f(-2.0f, 0.0f, -1.0f, 1.0f)).x, -1.0f, kEps);
    CHECK_NEAR((O * Vector4f(0.0f, 1.0f, -1.0f, 1.0f)).y, 1.0f, kEps);
    CHECK_NEAR((O * Vector4f(0.0f, -1.0f, -1.0f, 1.0f)).y, -1.0f, kEps);
    CHECK_NEAR((O * Vector4f(0.0f, 0.0f, -1.0f, 1.0f)).z, -1.0f, kEps);
    CHECK_NEAR((O * Vector4f(0.0f, 0.0f, -11.0f, 1.0f)).z, 1.0f, kEps);

    // 正交投影 w 恒为 1，没有透视缩放
    const Vector4f p = O * Vector4f(0.5f, 0.25f, -6.0f, 1.0f);
    CHECK_NEAR(p.w, 1.0f, kEps);
    CHECK_NEAR(p.x, 0.25f, kEps);
    CHECK_NEAR(p.y, 0.25f, kEps);
    CHECK_NEAR(p.z, 0.0f, kEps); // near=1 / far=11 的中点正好在 0
}

TEST(matrix, look_at) {
    // 相机在 +Z 看向原点：世界原点应落在视空间 (0, 0, -5)
    const Matrix4f V = Matrix4f::look_at(Vector3f(0.0f, 0.0f, 5.0f),
                                         Vector3f(0.0f, 0.0f, 0.0f),
                                         Vector3f(0.0f, 1.0f, 0.0f));
    CHECK_VEC3(V.transform_point(Vector3f(0.0f, 0.0f, 0.0f)), 0.0f, 0.0f, -5.0f, kEps);
    // 相机自身位置 -> 视空间原点
    CHECK_VEC3(V.transform_point(Vector3f(0.0f, 0.0f, 5.0f)), 0.0f, 0.0f, 0.0f, kEps);
    // 相机右侧的点 -> 视空间 +X；上方的点 -> +Y；更远的点 -> z 更负
    CHECK_NEAR(V.transform_point(Vector3f(2.0f, 0.0f, 0.0f)).x, 2.0f, kEps);
    CHECK_NEAR(V.transform_point(Vector3f(0.0f, 3.0f, 0.0f)).y, 3.0f, kEps);
    CHECK_NEAR(V.transform_point(Vector3f(0.0f, 0.0f, -2.0f)).z, -7.0f, kEps);

    // 从上往下看：由于 up 取世界 +Z，"世界上方"被投影到视空间 +Y，
    // 于是世界 +X 落到视空间 -X（右手_VIEW 系下的必然结果，不是错误）。
    const Matrix4f top_down = Matrix4f::look_at(Vector3f(0.0f, 5.0f, 0.0f),
                                                Vector3f(0.0f, 0.0f, 0.0f),
                                                Vector3f(0.0f, 0.0f, 1.0f));
    CHECK_NEAR(top_down.transform_point(Vector3f(0.0f, 0.0f, 0.0f)).z, -5.0f, kEps);
    CHECK_NEAR(top_down.transform_point(Vector3f(0.0f, 0.0f, 1.0f)).y, 1.0f, kEps);
    CHECK_NEAR(top_down.transform_point(Vector3f(1.0f, 0.0f, 0.0f)).x, -1.0f, kEps);

    // 视矩阵是刚体变换：逆就是它的转置旋转 + 反向平移
    const Matrix4f Vinv = V.inverse();
    CHECK_VEC3(Vinv.transform_point(V.transform_point(Vector3f(1.0f, 2.0f, -3.0f))),
               1.0f, 2.0f, -3.0f, kLooseEps);
}

TEST(matrix, basis_accessors) {
    const Matrix4f M = Matrix4f::translate(1.0f, 2.0f, 3.0f)
                     * Matrix4f::rotate(tc::to_radians(90.0f), Vector3f(0.0f, 1.0f, 0.0f));
    // 绕 Y 转 90° 后，局部 +X 指到世界 -Z（Z -> X 意味着 X -> -Z）
    CHECK_VEC3(M.right_vector(), 0.0f, 0.0f, -1.0f, kEps);
    CHECK_VEC3(M.up_vector(), 0.0f, 1.0f, 0.0f, kEps);
    // forward_vector 取第三列的相反方向
    CHECK_VEC3(M.forward_vector(), -1.0f, 0.0f, 0.0f, kEps);
    CHECK_VEC3(M.translation(), 1.0f, 2.0f, 3.0f, kEps);
    CHECK_UNIT_VEC3(M.right_vector(), kEps);
    CHECK_UNIT_VEC3(M.up_vector(), kEps);
    CHECK_UNIT_VEC3(M.forward_vector(), kEps);
}

TEST(matrix, billboard) {
    const Vector3f pos(1.0f, 2.0f, 3.0f);
    const Vector3f cam(1.0f, 2.0f, 10.0f);
    const Vector3f scale(2.0f, 3.0f, 4.0f);
    const Matrix4f b = gryce_engine::math::billboard_matrix(pos, scale, cam);

    // 平移与缩放被保留
    CHECK_VEC3(b.translation(), 1.0f, 2.0f, 3.0f, kEps);
    // 局部 +Z 指向相机
    const Vector3f z_axis = Vector3f(b(0, 2), b(1, 2), b(2, 2));
    CHECK_VEC3(z_axis, 0.0f, 0.0f, 4.0f, kEps);
    // 三个基向量互相正交，长度等于各自的缩放分量
    const Vector3f x_axis = Vector3f(b(0, 0), b(1, 0), b(2, 0));
    const Vector3f y_axis = Vector3f(b(0, 1), b(1, 1), b(2, 1));
    CHECK_NEAR(x_axis.length(), 2.0f, kEps);
    CHECK_NEAR(y_axis.length(), 3.0f, kEps);
    CHECK_NEAR(x_axis.normalized().dot(y_axis.normalized()), 0.0f, kEps);
    CHECK_NEAR(y_axis.normalized().dot(z_axis.normalized()), 0.0f, kEps);
}

// ===========================================================================
// suite: quaternion —— 轴角、旋转、插值、欧拉角、矩阵往返
// ===========================================================================
TEST(quaternion, identity_and_axis_angle) {
    const Quaternionf id = Quaternionf::identity();
    const Vector3f v(1.0f, 2.0f, 3.0f);
    CHECK_VEC3(id.rotate_vector(v), 1.0f, 2.0f, 3.0f, kEps);
    CHECK_NEAR(id.length(), 1.0f, kEps);
    CHECK_NEAR(id.w, 1.0f, kEps);

    const float k90 = tc::to_radians(90.0f);
    // 绕 +Z 90°：X -> Y（与 Matrix4f::rotate 的右手系约定一致）
    CHECK_VEC3(Quaternionf::from_axis_angle(Vector3f(0.0f, 0.0f, 1.0f), k90).rotate_vector(Vector3f(1.0f, 0.0f, 0.0f)),
               0.0f, 1.0f, 0.0f, kEps);
    // 绕 +Y 90°：Z -> X
    CHECK_VEC3(Quaternionf::from_axis_angle(Vector3f(0.0f, 1.0f, 0.0f), k90).rotate_vector(Vector3f(0.0f, 0.0f, 1.0f)),
               1.0f, 0.0f, 0.0f, kEps);
    // 绕 +X 90°：Y -> Z
    CHECK_VEC3(Quaternionf::from_axis_angle(Vector3f(1.0f, 0.0f, 0.0f), k90).rotate_vector(Vector3f(0.0f, 1.0f, 0.0f)),
               0.0f, 0.0f, 1.0f, kEps);

    // 轴上的向量不动
    const Vector3f axis(1.0f, 2.0f, 3.0f);
    CHECK_VEC3(Quaternionf::from_axis_angle(axis, tc::to_radians(37.0f)).rotate_vector(axis.normalized()),
               axis.normalized().x, axis.normalized().y, axis.normalized().z, kEps);

    // 旋转保长度
    const Quaternionf q = Quaternionf::from_axis_angle(Vector3f(0.3f, -1.1f, 0.6f), tc::to_radians(123.0f));
    CHECK_NEAR(q.length(), 1.0f, kEps);
    CHECK_NEAR(q.rotate_vector(Vector3f(4.0f, -2.0f, 7.0f)).length(),
               Vector3f(4.0f, -2.0f, 7.0f).length(), kEps);
}

TEST(quaternion, multiplication_and_inverse) {
    const Quaternionf a = Quaternionf::from_axis_angle(Vector3f(0.0f, 1.0f, 0.0f), tc::to_radians(40.0f));
    const Quaternionf b = Quaternionf::from_axis_angle(Vector3f(1.0f, 0.0f, 0.0f), tc::to_radians(-25.0f));
    const Vector3f v(0.5f, -1.5f, 2.0f);

    // 复合语义：a*b 表示先施加 b 再施加 a
    CHECK_VEC3((a * b).rotate_vector(v),
               a.rotate_vector(b.rotate_vector(v)).x,
               a.rotate_vector(b.rotate_vector(v)).y,
               a.rotate_vector(b.rotate_vector(v)).z, kEps);

    // 结合律
    const Quaternionf c = Quaternionf::from_axis_angle(Vector3f(0.0f, 0.0f, 1.0f), tc::to_radians(70.0f));
    const Vector3f lhs = ((a * b) * c).rotate_vector(v);
    const Vector3f rhs = (a * (b * c)).rotate_vector(v);
    CHECK_VEC3(lhs, rhs.x, rhs.y, rhs.z, kEps);

    // q * q⁻¹ = identity（共轭即单位四元数的逆）
    const Quaternionf inv = (a * b).conjugate();
    CHECK_VEC3(((a * b) * inv).rotate_vector(v), v.x, v.y, v.z, kEps);
    CHECK_VEC3((inv * (a * b)).rotate_vector(v), v.x, v.y, v.z, kEps);

    // 单位元
    CHECK_VEC3((Quaternionf::identity() * a).rotate_vector(v), a.rotate_vector(v).x,
               a.rotate_vector(v).y, a.rotate_vector(v).z, kEps);
}

TEST(quaternion, slerp) {
    const Quaternionf a = Quaternionf::identity();
    const Quaternionf b = Quaternionf::from_axis_angle(Vector3f(0.0f, 0.0f, 1.0f), tc::to_radians(90.0f));
    const Vector3f x(1.0f, 0.0f, 0.0f);

    // 端点
    CHECK_VEC3(Quaternionf::slerp(a, b, 0.0f).rotate_vector(x), 1.0f, 0.0f, 0.0f, kEps);
    CHECK_VEC3(Quaternionf::slerp(a, b, 1.0f).rotate_vector(x), 0.0f, 1.0f, 0.0f, kEps);

    // 中点：半速代入 obstacle -> 45°
    const float c45 = std::cos(tc::to_radians(45.0f));
    const float s45 = std::sin(tc::to_radians(45.0f));
    CHECK_VEC3(Quaternionf::slerp(a, b, 0.5f).rotate_vector(x), c45, s45, 0.0f, kEps);

    // 插值结果是单位四元数（等速角插值）
    CHECK_NEAR(Quaternionf::slerp(a, b, 0.35f).length(), 1.0f, kEps);
    // 匀速：t=0.25 应恰好转 22.5°
    CHECK_NEAR(Quaternionf::slerp(a, b, 0.25f).rotate_vector(x).y,
               std::sin(tc::to_radians(22.5f)), kEps);

    // 最短路径：把 b 取反应得到同一姿态
    const Quaternionf neg_b(-b.x, -b.y, -b.z, -b.w);
    const Vector3f via_neg = Quaternionf::slerp(a, neg_b, 0.5f).rotate_vector(x);
    CHECK_VEC3(via_neg, c45, s45, 0.0f, kEps);

    // 几乎重合时退化为 nlerp，仍应是合法单位四元数
    const Quaternionf tiny = Quaternionf::from_axis_angle(Vector3f(0.0f, 1.0f, 0.0f), 1e-4f);
    const Quaternionf blended = Quaternionf::slerp(a, tiny, 0.5f);
    CHECK_NEAR(blended.length(), 1.0f, kEps);
    CHECK_TRUE(std::fabs(blended.x) < 1e-3f && std::fabs(blended.z) < 1e-3f);
}

TEST(quaternion, euler_round_trip) {
    const Vector3f eulers[] = {
        Vector3f(0.0f, 0.0f, 0.0f),
        Vector3f(15.0f, -40.0f, 70.0f),
        Vector3f(-60.0f, 120.0f, 33.0f),
        Vector3f(42.0f, -179.0f, -20.0f),
    };
    for (const Vector3f& e : eulers) {
        const Quaternionf q = Quaternionf::from_euler(tc::to_radians(e.x),
                                                      tc::to_radians(e.y),
                                                      tc::to_radians(e.z));
        CHECK_NEAR(q.length(), 1.0f, kEps);
        const Vector3f back = q.to_euler();
        // 角度按 360° 归一化后再比较，避免 ±180 等价表示造成的假失败
        auto wrap = [](float d) {
            float v = std::fmod(d + 180.0f, 360.0f);
            if (v < 0.0f) v += 360.0f;
            return v - 180.0f;
        };
        CHECK_NEAR(wrap(back.x - e.x), 0.0f, kLooseEps);
        CHECK_NEAR(wrap(back.y - e.y), 0.0f, kLooseEps);
        CHECK_NEAR(wrap(back.z - e.z), 0.0f, kLooseEps);
    }
}

TEST(quaternion, to_matrix_and_back) {
    const Quaternionf samples[] = {
        Quaternionf::identity(),
        Quaternionf::from_axis_angle(Vector3f(0.0f, 1.0f, 0.0f), tc::to_radians(65.0f)),
        Quaternionf::from_axis_angle(Vector3f(1.0f, 2.0f, -3.0f), tc::to_radians(-140.0f)),
        Quaternionf::from_euler(tc::to_radians(10.0f), tc::to_radians(80.0f), tc::to_radians(-35.0f)),
    };
    for (const Quaternionf& q : samples) {
        const Matrix4f M = Matrix4f::from_quaternion(q);

        // 纯旋转矩阵：三个基向量正交且单位长
        CHECK_UNIT_VEC3(M.right_vector(), kEps);
        CHECK_UNIT_VEC3(M.up_vector(), kEps);
        CHECK_UNIT_VEC3(M.forward_vector(), kEps);
        CHECK_NEAR(M.right_vector().dot(M.up_vector()), 0.0f, kEps);
        CHECK_NEAR(M.up_vector().dot(M.forward_vector()), 0.0f, kEps);
        CHECK_VEC3(M.translation(), 0.0f, 0.0f, 0.0f, kEps);

        // 旋转矩阵不含 NonFinite
        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(M.m[i])) finite = false;
        }
        CHECK_TRUE(finite);

        // to_matrix() 必须与 rotate_vector 同一旋转方向：M * v == q 作用在 v 上
        const Vector3f v(0.7f, -2.2f, 3.1f);
        CHECK_VEC3(M.transform_vector(v), q.rotate_vector(v).x, q.rotate_vector(v).y,
                   q.rotate_vector(v).z, kEps);

        // 矩阵 -> 四元数往返应还原同一旋转（允许差一个符号）
        const Quaternionf rt = Quaternionf::from_rotation_matrix(M);
        CHECK_NEAR(rt.length(), 1.0f, kLooseEps);
        CHECK_VEC3(Matrix4f::from_quaternion(rt).transform_vector(v),
                   M.transform_vector(v).x, M.transform_vector(v).y, M.transform_vector(v).z,
                   kLooseEps);
    }
}

TEST(quaternion, from_rotation_matrix_matches_matrix_rotate) {
    // Matrix4f::rotate(angle, axis) 与同参数四元数必须逐元素一致，
    // 这是"列主序布局"最直接的回归锁：历史上 to_matrix 按行书写会整体转置。
    const struct { float angle; Vector3f axis; } samples[] = {
        {tc::to_radians(53.0f), Vector3f(0.0f, 0.0f, 1.0f)},
        {tc::to_radians(-37.0f), Vector3f(0.0f, 1.0f, 0.0f)},
        {tc::to_radians(160.0f), Vector3f(1.0f, 2.0f, -3.0f)},
    };
    for (const auto& s : samples) {
        const Quaternionf q = Quaternionf::from_axis_angle(s.axis, s.angle);
        const Matrix4f expected = Matrix4f::rotate(s.angle, s.axis);
        const Matrix4f actual = Matrix4f::from_quaternion(q);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                CHECK_NEAR(actual(r, c), expected(r, c), kEps);
            }
        }
        // 反向：由旋转矩阵取回四元数，应还原同一旋转
        const Quaternionf rt = Quaternionf::from_rotation_matrix(actual);
        CHECK_NEAR(rt.length(), 1.0f, kEps);
        const Vector3f v(0.3f, -1.7f, 2.4f);
        CHECK_VEC3(rt.rotate_vector(v), q.rotate_vector(v).x, q.rotate_vector(v).y,
                   q.rotate_vector(v).z, kLooseEps);
    }
}

// ===========================================================================
// suite: camera —— FPS 相机朝向、移动与投影
// ===========================================================================
TEST(camera, defaults) {
    Camera cam;
    CHECK_VEC3(cam.position(), 0.0f, 0.0f, 5.0f, kEps);
    // 默认 yaw = -90° -> 看向 -Z
    CHECK_VEC3(cam.forward(), 0.0f, 0.0f, -1.0f, kEps);
    CHECK_VEC3(cam.right(), 1.0f, 0.0f, 0.0f, kEps);
    CHECK_VEC3(cam.up(), 0.0f, 1.0f, 0.0f, kEps);
    CHECK_NEAR(cam.yaw(), -90.0f, kEps);
    CHECK_NEAR(cam.pitch(), 0.0f, kEps);
    CHECK_NEAR(cam.fov(), 60.0f, kEps);
    CHECK_NEAR(cam.near_plane(), 0.1f, kEps);
    CHECK_NEAR(cam.far_plane(), 100.0f, kEps);
    CHECK_UNIT_VEC3(cam.forward(), kEps);
    CHECK_UNIT_VEC3(cam.right(), kEps);
    CHECK_UNIT_VEC3(cam.up(), kEps);
    // 三个基构成右手正交系
    CHECK_NEAR(cam.right().cross(cam.forward()).dot(cam.up()), 1.0f, kEps);
}

TEST(camera, look_at_and_view_matrix) {
    Camera cam;
    cam.set_position(Vector3f(0.0f, 0.0f, 5.0f));
    cam.look_at(Vector3f::zero());
    // 看向原点（相机在 +Z）：forward = (0,0,-1)， yaw = -90°，pitch = 0°
    CHECK_VEC3(cam.forward(), 0.0f, 0.0f, -1.0f, kEps);
    CHECK_NEAR(cam.pitch(), 0.0f, kLooseEps);
    CHECK_NEAR(cam.yaw(), -90.0f, kLooseEps);

    const Matrix4f V = cam.get_view_matrix();
    CHECK_VEC3(V.transform_point(Vector3f::zero()), 0.0f, 0.0f, -5.0f, kEps);
    CHECK_VEC3(V.transform_point(cam.position()), 0.0f, 0.0f, 0.0f, kEps);

    // 从侧上方看原点
    Camera cam2;
    cam2.set_position(Vector3f(10.0f, 0.0f, 0.0f));
    cam2.look_at(Vector3f::zero());
    CHECK_VEC3(cam2.forward(), -1.0f, 0.0f, 0.0f, kEps);
    CHECK_VEC3(cam2.get_view_matrix().transform_point(Vector3f::zero()), 0.0f, 0.0f, -10.0f, kEps);

    // 俯视
    Camera cam3;
    cam3.set_position(Vector3f(0.0f, 8.0f, 0.0f));
    cam3.look_at(Vector3f::zero());
    CHECK_NEAR(cam3.pitch(), -90.0f, kLooseEps);
    CHECK_VEC3(cam3.forward(), 0.0f, -1.0f, 0.0f, kEps);
}

TEST(camera, movement) {
    // 前进：沿相机 forward 走 speed * dt
    Camera cam;
    cam.set_position(Vector3f::zero());
    cam.set_move_speed(5.0f);
    cam.update(1.0f, true, false, false, false, false, false, false, 0.0f, 0.0f);
    CHECK_VEC3(cam.position(), 0.0f, 0.0f, -5.0f, kEps);

    // 后退
    Camera cam2;
    cam2.set_position(Vector3f::zero());
    cam2.set_move_speed(5.0f);
    cam2.update(1.0f, false, true, false, false, false, false, false, 0.0f, 0.0f);
    CHECK_VEC3(cam2.position(), 0.0f, 0.0f, 5.0f, kEps);

    // 加速：speed * sprint_multiplier
    Camera cam3;
    cam3.set_position(Vector3f::zero());
    cam3.set_move_speed(4.0f);
    cam3.set_sprint_multiplier(2.5f);
    cam3.update(1.0f, true, false, false, false, false, false, true, 0.0f, 0.0f);
    CHECK_VEC3(cam3.position(), 0.0f, 0.0f, -10.0f, kEps);

    // 斜向移动要归一化，不能比直线更快
    Camera cam4;
    cam4.set_position(Vector3f::zero());
    cam4.set_move_speed(5.0f);
    cam4.update(1.0f, true, false, false, true, false, false, false, 0.0f, 0.0f);
    CHECK_NEAR(cam4.position().length(), 5.0f, kEps);
    const float root2 = 0.70710678f;
    CHECK_VEC3(cam4.position(), root2 * 5.0f, 0.0f, -root2 * 5.0f, kEps);

    // 垂直移动用世界上方向
    Camera cam5;
    cam5.set_position(Vector3f::zero());
    cam5.set_move_speed(2.0f);
    cam5.update(1.0f, false, false, false, false, true, false, false, 0.0f, 0.0f);
    CHECK_VEC3(cam5.position(), 0.0f, 2.0f, 0.0f, kEps);

    // 前后同时按下应相互抵消
    Camera cam6;
    cam6.set_position(Vector3f(3.0f, 4.0f, 5.0f));
    cam6.update(1.0f, true, true, false, false, false, false, false, 0.0f, 0.0f);
    CHECK_VEC3(cam6.position(), 3.0f, 4.0f, 5.0f, kEps);

    // dt 线性：两倍时间走两倍距离
    Camera cam7;
    cam7.set_position(Vector3f::zero());
    cam7.set_move_speed(3.0f);
    cam7.update(2.0f, true, false, false, false, false, false, false, 0.0f, 0.0f);
    CHECK_VEC3(cam7.position(), 0.0f, 0.0f, -6.0f, kEps);
}

TEST(camera, mouse_look_and_pitch_clamp) {
    // 鼠标横向输入改变 yaw，灵敏度 0.1
    Camera cam;
    cam.set_mouse_sensitivity(0.1f);
    cam.update(0.0f, false, false, false, false, false, false, false, 100.0f, 0.0f);
    CHECK_NEAR(cam.yaw(), -90.0f + 10.0f, kEps);
    cam.update(0.0f, false, false, false, false, false, false, false, -100.0f, 0.0f);
    CHECK_NEAR(cam.yaw(), -90.0f, kEps);

    // pitch 被夹在 [-89, 89]
    Camera cam2;
    cam2.set_mouse_sensitivity(0.1f);
    cam2.update(0.0f, false, false, false, false, false, false, false, 0.0f, 5000.0f);
    CHECK_NEAR(cam2.pitch(), 89.0f, kEps);
    cam2.update(0.0f, false, false, false, false, false, false, false, 0.0f, -10000.0f);
    CHECK_NEAR(cam2.pitch(), -89.0f, kEps);
    cam2.set_pitch(135.0f);
    CHECK_NEAR(cam2.pitch(), 89.0f, kEps);
    cam2.set_pitch(-135.0f);
    CHECK_NEAR(cam2.pitch(), -89.0f, kEps);

    // 俯仰 90° 附近 roll 不退化：right 仍为单位且与 forward 正交
    CHECK_UNIT_VEC3(cam2.right(), kEps);
    CHECK_NEAR(cam2.right().dot(cam2.forward()), 0.0f, kEps);

    // 抬头 45° 时 forward.y = sin(45°)
    Camera cam3;
    cam3.set_yaw(-90.0f);
    cam3.set_pitch(45.0f);
    CHECK_NEAR(cam3.forward().y, 0.70710678f, kEps);
    CHECK_NEAR(cam3.forward().z, -0.70710678f, kEps);
    CHECK_UNIT_VEC3(cam3.forward(), kEps);
}

TEST(camera, projection_matrix) {
    Camera cam;
    cam.set_fov(90.0f);
    cam.set_near_far(0.5f, 50.0f);
    const Matrix4f P = cam.get_projection_matrix(1.0f);
    // 90° FOV：视锥边缘 45°，故 (y = d) 处映射到 NDC y = 1
    const Vector4f top = P * Vector4f(0.0f, 5.0f, -5.0f, 1.0f);
    CHECK_NEAR(top.y / top.w, 1.0f, kEps);
    const Vector4f cn = P * Vector4f(0.0f, 0.0f, -0.5f, 1.0f);
    CHECK_NEAR(cn.z / cn.w, -1.0f, kEps);
    const Vector4f cf = P * Vector4f(0.0f, 0.0f, -50.0f, 1.0f);
    CHECK_NEAR(cf.z / cf.w, 1.0f, kEps);

    // aspect 要透传：无参版本用当前 aspect
    cam.set_aspect(2.0f);
    const Matrix4f P2 = cam.get_projection_matrix();
    const Vector4f edge = P2 * Vector4f(10.0f, 5.0f, -5.0f, 1.0f);
    CHECK_NEAR(edge.x / edge.w, 1.0f, kEps);
    CHECK_NEAR(edge.y / edge.w, 1.0f, kEps);
    CHECK_NEAR(cam.aspect(), 2.0f, kEps);
}

// ===========================================================================
// suite: ray —— 射线求值、slab 法 AABB 求交、屏幕坐标反投影
// ===========================================================================
TEST(ray, ray_at) {
    const Ray r{Vector3f(1.0f, 2.0f, 3.0f), Vector3f(0.0f, 0.0f, -1.0f)};
    CHECK_VEC3(r.at(0.0f), 1.0f, 2.0f, 3.0f, kEps);
    CHECK_VEC3(r.at(5.0f), 1.0f, 2.0f, -2.0f, kEps);
    CHECK_NEAR(r.origin.distance(r.at(3.0f)), 3.0f, kEps);
}

TEST(ray, aabb_hit_and_miss) {
    const Vector3f bmin(-1.0f, -1.0f, -1.0f);
    const Vector3f bmax(1.0f, 1.0f, 1.0f);

    // 正面命中：起点 (-5,0,0) 沿 +X，进入距离 4
    float t = 0.0f;
    Ray r{Vector3f(-5.0f, 0.0f, 0.0f), Vector3f(1.0f, 0.0f, 0.0f)};
    CHECK_TRUE(gryce_engine::math::ray_intersect_aabb(r, bmin, bmax, t));
    CHECK_NEAR(t, 4.0f, kEps);
    CHECK_VEC3(r.at(t), -1.0f, 0.0f, 0.0f, kEps);

    // 斜向命中：从 (2,2,2) 沿体对角线朝原点，落在角点 (1,1,1)
    Ray r2{Vector3f(2.0f, 2.0f, 2.0f), Vector3f(-1.0f, -1.0f, -1.0f).normalized()};
    CHECK_TRUE(gryce_engine::math::ray_intersect_aabb(r2, bmin, bmax, t));
    CHECK_NEAR(t, std::sqrt(3.0f), kEps);
    CHECK_VEC3(r2.at(t), 1.0f, 1.0f, 1.0f, kEps);

    // 起点在盒内：返回负的 tmin（既有语义）
    Ray r3{Vector3f::zero(), Vector3f(1.0f, 0.0f, 0.0f)};
    CHECK_TRUE(gryce_engine::math::ray_intersect_aabb(r3, bmin, bmax, t));
    CHECK_NEAR(t, -1.0f, kEps);

    // 平行且在 slab 之外 -> 不相交
    Ray r4{Vector3f(0.0f, 5.0f, 0.0f), Vector3f(1.0f, 0.0f, 0.0f)};
    CHECK_TRUE(!gryce_engine::math::ray_intersect_aabb(r4, bmin, bmax, t));

    // 射线穿过但盒子在其后方 -> 不相交
    Ray r5{Vector3f(5.0f, 0.0f, 0.0f), Vector3f(1.0f, 0.0f, 0.0f)};
    CHECK_TRUE(!gryce_engine::math::ray_intersect_aabb(r5, bmin, bmax, t));

    // 擦着旁边过去 -> 不相交
    Ray r6{Vector3f(-5.0f, 1.5f, 0.0f), Vector3f(1.0f, 0.0f, 0.0f)};
    CHECK_TRUE(!gryce_engine::math::ray_intersect_aabb(r6, bmin, bmax, t));

    // 非均匀盒子
    const Vector3f lo(0.0f, 0.0f, 0.0f);
    const Vector3f hi(4.0f, 2.0f, 6.0f);
    Ray r7{Vector3f(2.0f, 1.0f, -10.0f), Vector3f(0.0f, 0.0f, 1.0f)};
    CHECK_TRUE(gryce_engine::math::ray_intersect_aabb(r7, lo, hi, t));
    CHECK_NEAR(t, 10.0f, kEps);
}

TEST(ray, screen_ndc_to_ray) {
    Camera cam;
    cam.set_position(Vector3f(0.0f, 0.0f, 5.0f));
    cam.set_aspect(16.0f / 9.0f);
    cam.set_fov(60.0f);
    cam.set_near_far(0.1f, 100.0f);
    // 默认 yaw = -90°，即看向 -Z
    const Matrix4f view_proj = cam.get_projection_matrix() * cam.get_view_matrix();
    const Matrix4f inv_vp = view_proj.inverse();

    // 屏幕中心 -> 相机正前方
    const Ray center = gryce_engine::math::screen_ndc_to_ray(0.0f, 0.0f, inv_vp);
    CHECK_NEAR(center.direction.dot(cam.forward()), 1.0f, kLooseEps);
    CHECK_VEC3(center.origin, 0.0f, 0.0f, 4.9f, kLooseEps);

    // 射线上近平面处的点应落在 NDC z = -1
    const Vector4f reproj = view_proj * Vector4f(center.origin, 1.0f);
    CHECK_NEAR(reproj.z / reproj.w, -1.0f, kLooseEps);

    // 右上角 -> 方向应当偏右上（NDC y 向上）
    const Ray corner = gryce_engine::math::screen_ndc_to_ray(1.0f, 1.0f, inv_vp);
    CHECK_TRUE(corner.direction.x > 0.0f);
    CHECK_TRUE(corner.direction.y > 0.0f);
    CHECK_UNIT_VEC3(corner.direction, kLooseEps);
    CHECK_UNIT_VEC3(center.direction, kLooseEps);

    // 反投影回来的射线中点应仍在屏幕上同一位置
    const Vector4f back = view_proj * Vector4f(corner.at(20.0f), 1.0f);
    CHECK_NEAR(back.x / back.w, 1.0f, kLooseEps);
    CHECK_NEAR(back.y / back.w, 1.0f, kLooseEps);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            wanted.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            std::printf("usage: %s [--suite <name>]... [--list]\n", argv[0]);
            return 2;
        }
    }

    // 稳定的 suite 顺序（按注册顺序去重）
    std::vector<std::string> suites;
    for (const Case& c : registry()) {
        bool dup = false;
        for (const std::string& s : suites) {
            if (s == c.suite) { dup = true; break; }
        }
        if (!dup) suites.push_back(c.suite);
    }

    if (list_only) {
        for (const std::string& s : suites) {
            std::size_t n = 0;
            for (const Case& c : registry()) {
                if (c.suite == s) ++n;
            }
            std::printf("%-12s %zu cases\n", s.c_str(), n);
        }
        return 0;
    }

    int cases_total = 0;
    int cases_failed = 0;
    for (const std::string& s : suites) {
        if (!wanted.empty()) {
            bool sel = false;
            for (const std::string& w : wanted) {
                if (w == s) { sel = true; break; }
            }
            if (!sel) continue;
        }
        std::printf("[ SUITE ] %s\n", s.c_str());
        for (const Case& c : registry()) {
            if (c.suite != s) continue;
            begin_case(c.suite, c.name);
            c.fn();
            ++cases_total;
            if (g_case_failed > 0) {
                ++cases_failed;
                std::printf("  [FAIL] %s (%d/%d checks failed)\n",
                            c.name.c_str(), g_case_failed, g_case_checks);
            } else {
                std::printf("  [ OK  ] %s (%d checks)\n", c.name.c_str(), g_case_checks);
            }
        }
    }

    const int failed_checks = g_checks_failed;
    std::printf("\n--------------------------------------------------------------\n");
    std::printf("checks: %d total, %d passed, %d failed\n",
                g_checks, g_checks - failed_checks, failed_checks);
    std::printf("cases : %d total, %d passed, %d failed\n",
                cases_total, cases_total - cases_failed, cases_failed);
    if (cases_failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
