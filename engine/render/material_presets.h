#pragma once

// ---------------------------------------------------------------------------
// material_presets.h — 渲染材质预设表
//
// 与参考工程 components/physical_material.h 的物理材质预设同构：一张
// constexpr 表 + Material::apply_preset(name)。区别是这里描述的是渲染参数
// （PBR 基色/粗糙度/金属度 + 清漆/织物/各向异性 + 混合模式）。
//
// 语义（与参考工程 PhysicalMaterial 保持一致）：
//   材质 JSON 里写了 preset_name 时，加载后以预设表为准重新套用一遍，
//   这样修改这张表就能刷新所有引用它的旧场景。
//   需要逐实例微调的材质请不要写 preset_name，直接写具体参数。
// ---------------------------------------------------------------------------

namespace gryce_engine::render {

struct MaterialPreset {
    const char* name;            // 稳定标识（场景/脚本/API 用，勿随意改名）
    const char* label;           // 编辑器显示名
    float albedo[3];
    float roughness;
    float metallic;
    float ao;
    float emissive[3];           // 自发光颜色（叠加在光照结果之上）
    float opacity;               // <1 时材质自动转半透明
    int blend_mode;              // 0=Opaque, 1=Blend
    bool two_sided;
    float clearcoat;             // 清漆（车漆/塑料）
    float clearcoat_roughness;
    float sheen;                 // 织物光泽
    float sheen_tint[3];
    float anisotropy;            // -1~1 各向异性（拉丝金属）
};

constexpr MaterialPreset k_material_presets[] = {
    // name            label         albedo                rough  metal   ao   emissive            opac  blend 2side clear  ccRgh sheen  sheenTint          aniso
    {"Default",        "默认",       {0.80f, 0.80f, 0.80f}, 0.50f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Concrete",       "混凝土",     {0.55f, 0.55f, 0.53f}, 0.92f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Wood",           "木头",       {0.42f, 0.28f, 0.16f}, 0.70f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.05f, 0.30f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Rubber",         "橡胶",       {0.12f, 0.12f, 0.13f}, 0.95f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"PlasticMatte",   "哑光塑料",   {0.85f, 0.85f, 0.86f}, 0.85f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"PlasticGlossy",  "高光塑料",   {0.90f, 0.90f, 0.92f}, 0.18f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.60f, 0.05f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Ceramic",        "陶瓷",       {0.92f, 0.90f, 0.86f}, 0.25f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.85f, 0.03f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"CarPaint",       "车漆",       {0.70f, 0.05f, 0.06f}, 0.30f, 0.60f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 1.00f, 0.03f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Fabric",         "织物",       {0.40f, 0.35f, 0.34f}, 0.90f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, true,  0.00f, 0.10f, 0.8f, {0.90f, 0.85f, 0.80f}, 0.00f},
    {"Aluminum",       "铝",         {0.91f, 0.92f, 0.92f}, 0.28f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Steel",          "钢",         {0.56f, 0.57f, 0.58f}, 0.35f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Chrome",         "镀铬",       {0.90f, 0.92f, 0.95f}, 0.05f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Gold",           "黄金",       {1.00f, 0.77f, 0.34f}, 0.20f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"Copper",         "紫铜",       {0.95f, 0.64f, 0.54f}, 0.30f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"BrushedMetal",   "拉丝金属",   {0.75f, 0.75f, 0.78f}, 0.45f, 1.00f, 1.00f, {0.0f, 0.0f, 0.0f},   1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.65f},
    {"Glass",          "玻璃",       {0.90f, 0.95f, 0.98f}, 0.05f, 0.00f, 1.00f, {0.0f, 0.0f, 0.0f},   0.35f, 1, true,  0.40f, 0.02f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
    {"EmissiveLamp",   "自发光",     {1.00f, 1.00f, 1.00f}, 0.50f, 0.00f, 1.00f, {1.0f, 0.90f, 0.70f}, 1.0f, 0, false, 0.00f, 0.10f, 0.0f, {1.0f, 1.0f, 1.0f},  0.00f},
};

constexpr int k_material_preset_count =
    static_cast<int>(sizeof(k_material_presets) / sizeof(k_material_presets[0]));

// 按名称查表；未命中返回 nullptr。
constexpr const MaterialPreset* find_material_preset(const char* name) {
    if (!name || !name[0]) return nullptr;
    for (int i = 0; i < k_material_preset_count; ++i) {
        const char* a = k_material_presets[i].name;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return &k_material_presets[i];
    }
    return nullptr;
}

} // namespace gryce_engine::render
