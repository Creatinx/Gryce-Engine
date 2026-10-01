#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。
// 输出到独立的 composite 目标（不是场景色附件），因此读取场景色是安全的；
// 结果再由调用方整体拷回 HDR 目标。这样 GL / Vulkan 行为完全一致：
// 不依赖 draw 前的混合状态（Vulkan 的混合在管线创建时定死），
// 也不依赖目标是否会被重新绑定清空（Vulkan 的 render pass 是 loadOp=CLEAR）。
uniform sampler2D uColorTex; // HDR 场景颜色（合成前；A = 该像素的 IBL 镜面项亮度）
uniform sampler2D uSSRTex;   // SSR 反射颜色（模糊后；A = 命中覆盖度）
uniform float uSSREnvFallback; // 0 = 旧行为（叠加）；1 = 命中处替换 IBL（推荐）
uniform float uSSRDebugMode;   // 0=正常合成 1=原始反射 2=覆盖度 3=IBL 镜面项

void main() {
    vec4 scene = texture(uColorTex, vTexCoord);
    vec4 ssr = texture(uSSRTex, vTexCoord);

    if (uSSRDebugMode > 0.5) {
        if (uSSRDebugMode < 1.5) { FragColor = vec4(ssr.rgb, 1.0); return; }         // 原始反射
        if (uSSRDebugMode < 2.5) { FragColor = vec4(vec3(ssr.a), 1.0); return; }     // 命中覆盖度
        FragColor = vec4(vec3(scene.a), 1.0);                                        // IBL 镜面项
        return;
    }

    // ---- 按 BRDF 权重替换 IBL 镜面项 ----
    //
    // 旧实现是 scene*(1-cov*env_ratio) + ssr.rgb：移除那部分按 env_ratio 缩放，
    // 补进来的反射颜色却是**全强度**的。于是红塑料（F0≈0.04）也会被加上一整份
    // 明亮地面色 —— 金属之外的所有材质都发白，命中/未命中交界处再叠一层亮度突跳。
    //
    // 正确关系：镜面项 = BRDF 权重 × 反射方向的入射辐照（IBL 用环境贴图，
    // SSR 用屏幕空间命中颜色）。所以这里做三件事：
    //   1) 用 scene.a（pbr pass 写入的 IBL 镜面项亮度 / 整像素亮度）估计镜面权重；
    //   2) 移除 scene.rgb * env_ratio 这一份 IBL 镜面能量；
    //   3) 补上同权重、但带镜面色相的屏幕空间反射：
    //      spec_hue = spec_rgb / 亮度(spec_rgb)，单位亮度、保留金属色相
    //      （金块的反射因此仍是金色，铬块是白色，能量与移除项一致）。
    float scene_lum = dot(scene.rgb, vec3(0.2126, 0.7152, 0.0722));
    float env_ratio = scene_lum > 1e-4 ? clamp(scene.a / scene_lum, 0.0, 1.0) : 0.0;
    vec3  spec_rgb  = scene.rgb * env_ratio;
    vec3  spec_hue  = scene.a > 1e-5 ? spec_rgb / scene.a : vec3(1.0);

    float cover = clamp(ssr.a, 0.0, 1.0);
    float replace = clamp(uSSREnvFallback, 0.0, 1.0);
    // 补入项的强度也必须带 env_ratio：env_ratio 就是"这个像素里镜面项占多少"，
    // 金属 ≈1（反射是整像素 → 完整镜面），红塑料 ≈0.01（反射只有百分之几）。
    // 少了这个因子，非金属会凭空多出一整份明亮反射（实测红方块因此被改动 23%，
    // 比铬方块还夸张），也就是用户看到的"材质发白、像塑料薄膜"。
    // env_fallback=0 时退回旧行为（只叠加、不替换），故用 mix 插值。
    float add_w = cover * mix(1.0, env_ratio, replace);
    vec3 color = scene.rgb
               - spec_rgb * (cover * replace)
               + spec_hue * ssr.rgb * add_w;
    FragColor = vec4(color, 1.0);
}
