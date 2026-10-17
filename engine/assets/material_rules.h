#pragma once

// ---------------------------------------------------------------------------
// material_rules.h — 模型文件材质 → PBR 参数（格式无关的推导规则）
//
// 导入模型时"这块表面怎么跟光互动"只能靠文件里的材质数据推：
//   * glTF / 新一点的 FBX 直接给 metallicFactor、roughnessFactor（金属度工作流）
//   * 3ds Max / Maya 导出的老 FBX、OBJ MTL 只有镜面-光泽（Kd/Ks/Ns，镜面工作流）
//   * Collada / PLY / STL 往往什么都不给
// 各格式把自己的字段读出来后统一填 SurfaceInput，交给 resolve_surface()
// 换成引擎要用的 PBR 参数 + 表面类型。所有格式（assimp / 内置 OBJ / 以后新增的）
// 共用这一份规则，避免"换个格式导入，同一个模型变成另一种材质"。
//
// 规则本身不碰 IO / 日志 / GPU，只做数值推导 —— 因此可以直接编进
// GryceMathTests 做纯数值单测（ctest 里的 material suite）。
// ---------------------------------------------------------------------------

#include "math/math.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// SurfaceType — 表面类型（决定这套材质怎么参与光照）
//
// 不是"美术标签"，而是渲染分支：金属没有漫反射、半透明要走混合 pass、
// 自发光不依赖光源。导入时按文件数据推出来，日志里会打出来便于排查
// "为什么这块石头看起来像铁"。
// ---------------------------------------------------------------------------
enum class SurfaceType {
    Dielectric = 0,  // 非金属：石头/木头/混凝土/漆面/布料…（F0 ≈ 0.04，漫反射为主）
    Metal,           // 金属：基色即 F0，几乎没有漫反射，靠 IBL/SSR 反射出内容
    Transparent,     // 半透明：alpha 混合 + 双面（玻璃/纱帘/树叶卡片）
    Emissive,        // 自发光：叠加 emissive，不依赖光源
};

// 稳定名称（日志/工具/序列化用，勿随意改名）
const char* surface_type_name(SurfaceType type);

// ---------------------------------------------------------------------------
// SurfaceInput — 从文件里读到的原始表面数据（各格式填这个）
// ---------------------------------------------------------------------------
struct SurfaceInput {
    // 基色（线性空间）。多数格式给的是 sRGB 数值，这里按原值传入，
    // 由纹理/着色器负责色彩空间；标量基色直接使用。
    math::Vector3f base_color = math::Vector3f(0.8f, 0.8f, 0.8f);
    // 镜面颜色（spec-gloss 工作流的 Ks / F0）。仅当 has_specular 为真时可信。
    math::Vector3f specular_color = math::Vector3f(1.0f, 1.0f, 1.0f);
    math::Vector3f emissive = math::Vector3f::zero();

    float opacity = 1.0f;
    bool has_opacity = false;

    // ---- PBR 因子（金属度工作流，优先于下面的 spec-gloss 字段）----
    bool has_roughness = false;
    float roughness = 0.5f;
    bool has_metallic = false;
    float metallic = 0.0f;

    // ---- spec-gloss 工作流 ----
    // Blinn-Phong 高光指数（OBJ 的 Ns、FBX 的 Shininess）。0 = 极粗糙。
    bool has_shininess = false;
    float shininess = 0.0f;
    // 文件里**确实写了**镜面颜色（不是导出器/加载器的默认填充值）。
    // FBX 的 phong 材质即使没写 specular，assimp 也会填 0.9 灰，
    // 把它当成真镜面会把整场景误判成金属 —— 调用方必须自己把关。
    bool has_specular = false;
    // 镜面强度（FBX SpecularFactor / glTF 无此项），用于把镜面颜色压回 [0,1] 能量。
    bool has_specular_strength = false;
    float specular_strength = 1.0f;

    bool two_sided = false;

    // 是否带基色贴图。带了贴图时 base_color 通常只是"贴图乘子"（常为 0.5 灰），
    // 不能拿它当漫反射强度来反推金属度，否则有贴图的老 FBX 会被整体算成金属。
    bool has_base_color_map = false;
    // 是否带镜面贴图（镜面工作流下的反射贴图）
    bool has_specular_map = false;
};

// ---------------------------------------------------------------------------
// SurfaceResult — 引擎侧的 PBR 参数
// ---------------------------------------------------------------------------
struct SurfaceResult {
    math::Vector3f base_color = math::Vector3f(0.8f, 0.8f, 0.8f);
    float roughness = 0.5f;
    float metallic = 0.0f;
    float opacity = 1.0f;
    bool blend = false;  // 走 alpha 混合 pass
    bool two_sided = false;
    math::Vector3f emissive = math::Vector3f::zero();
    SurfaceType type = SurfaceType::Dielectric;
};

// 相对亮度（Rec.709 权重），金属度/自发光判定用。
float surface_luminance(const math::Vector3f& color);

// Blinn-Phong 高光指数 → GGX 粗糙度。
//   Blinn 指数 n 与 GGX 的 α 满足 α² = 2/(n+2)（同一高光半角分布），
//   Disney 粗糙度 r 与 α 的关系是 α = r²，故 r = (2/(n+2))^(1/4)。
// 旧的 sqrt(2/(n+2)) 直接把 α 当 r 用，高光指数越高偏得越离谱
// （n=1000：正确 r≈0.21，旧公式给 0.045 → 表面比文件写的亮得多，
// 整模型泛出金属般的镜面感）。
float shininess_to_roughness(float shininess);

// 推导规则（见 .cpp 里的注释）。纯函数、无副作用。
SurfaceResult resolve_surface(const SurfaceInput& input);

} // namespace gryce_engine::assets
