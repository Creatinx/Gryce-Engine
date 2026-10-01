#version 450 core

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 0 = uColorTex (kTonemapHDR), 10 = uSSRTex (kSSRTexture)
layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 10) uniform sampler2D uSSRTex;

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，144 字节；只用到尾部字段）
layout(push_constant) uniform PushConstants {
    mat4 view;              // +0
    vec3 camera_pos;        // +64
    vec2 screen_size;       // +80
    float near_plane;       // +88
    float far_plane;        // +92
    float tan_half_fov;     // +96
    float aspect;           // +100
    float max_roughness;    // +104
    int max_steps;          // +108
    float thickness;        // +112
    float bilateral_filter; // +116
    vec2 texel_size;        // +120
    float env_fallback;     // +128
    float _pad_tail[3];     // +132
} pc;

void main() {
    vec4 scene = texture(uColorTex, vUV);
    vec4 ssr = texture(uSSRTex, vUV);

    if (pc._pad_tail[0] > 0.5) {
        if (pc._pad_tail[0] < 1.5) { FragColor = vec4(ssr.rgb, 1.0); return; }
        if (pc._pad_tail[0] < 2.5) { FragColor = vec4(vec3(ssr.a), 1.0); return; }
        FragColor = vec4(vec3(scene.a), 1.0);
        return;
    }
    // 与 GL 版逐行一致：按 BRDF 权重替换 IBL 镜面项（说明见 ssr_composite.frag）。
    // 关键点是移除项与补入项用同一个权重，且补入项带镜面色相，
    // 否则金属以外的材质会被叠加一整份明亮反射而发白。
    float scene_lum = dot(scene.rgb, vec3(0.2126, 0.7152, 0.0722));
    float env_ratio = scene_lum > 1e-4 ? clamp(scene.a / scene_lum, 0.0, 1.0) : 0.0;
    vec3  spec_rgb = scene.rgb * env_ratio;
    vec3  spec_hue = scene.a > 1e-5 ? spec_rgb / scene.a : vec3(1.0);

    // 与 GL 版一致：补入项强度同样带 env_ratio（金属≈1 完整镜面，
    // 非金属≈0.01 只加一点点反射），env_fallback=0 时退回旧的纯叠加行为。
    float cover = clamp(ssr.a, 0.0, 1.0);
    float replace = clamp(pc.env_fallback, 0.0, 1.0);
    float add_w = cover * mix(1.0, env_ratio, replace);
    vec3 color = scene.rgb
               - spec_rgb * (cover * replace)
               + spec_hue * ssr.rgb * add_w;
    FragColor = vec4(color, 1.0);
}
