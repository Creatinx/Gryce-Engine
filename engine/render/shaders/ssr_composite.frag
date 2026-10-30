#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。
// 输出到独立的 composite 目标（不是场景色附件），因此读取场景色是安全的；
// 结果再由调用方整体拷回 HDR 目标。这样 GL / Vulkan 行为完全一致：
// 不依赖 draw 前的混合状态（Vulkan 的混合在管线创建时定死），
// 也不依赖目标是否会被重新绑定清空（Vulkan 的 render pass 是 loadOp=CLEAR）。
uniform sampler2D uColorTex; // HDR 场景颜色（A = 该像素的镜面反射率 ρ）
uniform sampler2D uSSRTex;   // SSR 反射颜色（模糊后；A = 命中覆盖度）
uniform float uSSREnvFallback; // 反射替换强度（1 = 完全替换 IBL 镜面项）
uniform float uSSRDebugMode;   // 0=正常合成 1=原始反射 2=覆盖度 3=反射率

void main() {
    vec4 scene = texture(uColorTex, vTexCoord);
    vec4 ssr = texture(uSSRTex, vTexCoord);

    if (uSSRDebugMode > 0.5) {
        if (uSSRDebugMode < 1.5) { FragColor = vec4(ssr.rgb, 1.0); return; }         // 原始反射
        if (uSSRDebugMode < 2.5) { FragColor = vec4(vec3(ssr.a), 1.0); return; }     // 命中覆盖度
        FragColor = vec4(vec3(scene.a), 1.0);                                        // 反射率
        return;
    }

    // ---- 按反射率混合，而不是按"镜面项占总亮度的比例" ----
    //
    // scene.a 是 pbr pass 写入的 **反射率** ρ = F0·(BRDF LUT 的 A) + B（含 AO），
    // 它是材质自身的属性：铬 ρ≈1、花岗岩 ρ≈0.05、掠射角的绝缘体 ρ 会抬到 0.3~1。
    //
    // 早先的实现用 ρ' = 镜面亮度 / 整像素亮度做权重，再补入一份**全强度**反射色。
    // 那个比值在只有环境光照的暗面上同样接近 1（分子分母一起被 IBL 强度缩放），
    // 于是砖墙、暗面在斜视角下被整块换成了明亮的反射内容 —— 画面里所有东西都像
    // 抛光瓷砖，这就是"按 SSR/RT 的表现看物体应该全是镜面"的根因。
    //
    // 现在是同一权重进出：mix(scene, hue*反射亮度, 覆盖度·ρ)，
    // 加进去的能量按 ρ 缩放，暗面不会被环境亮度"放大"成镜子。
    float rho = clamp(scene.a, 0.0, 1.0);
    float cover = clamp(ssr.a, 0.0, 1.0);
    float w = cover * rho * clamp(uSSREnvFallback, 0.0, 1.0);
    if (w <= 0.0) {
        FragColor = vec4(scene.rgb, 1.0);
        return;
    }

    float scene_lum = dot(scene.rgb, vec3(0.2126, 0.7152, 0.0722));
    // 单位亮度色相：金属保留自身颜色（金块反射仍是金色），绝缘体近似中性。
    vec3  hue = scene_lum > 1e-4 ? scene.rgb / scene_lum : vec3(1.0);
    float refl_lum = dot(ssr.rgb, vec3(0.2126, 0.7152, 0.0722));

    // 反射项只取反射内容的**亮度**，色相用表面自己的 —— 反射的强弱由反射面决定，
    // 而不是被反射物。直接用 ssr.rgb 会把命中物体的颜色整块搬过来，
    // 与"按表面 BRDF 加权"相矛盾。
    FragColor = vec4(mix(scene.rgb, hue * refl_lum, w), 1.0);
}
