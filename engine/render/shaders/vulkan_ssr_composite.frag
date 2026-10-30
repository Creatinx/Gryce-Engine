#version 450 core

// SSR 合成：把反射颜色叠加回 HDR 场景颜色（与 ssr_composite.frag 逐行等价）。

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
    int probe_valid;        // +132（trace 用；composite 忽略，但必须占位保证偏移）
    float debug_view;       // +136 SSR 调试视图模式
    float _pad_tail[1];     // +140 补齐 16 字节
} pc;

void main() {
    vec4 scene = texture(uColorTex, vUV);
    vec4 ssr = texture(uSSRTex, vUV);

    if (pc.debug_view > 0.5) {
        if (pc.debug_view < 1.5) { FragColor = vec4(ssr.rgb, 1.0); return; }
        if (pc.debug_view < 2.5) { FragColor = vec4(vec3(ssr.a), 1.0); return; }
        FragColor = vec4(vec3(scene.a), 1.0);
        return;
    }

    // 与 GL 版逐行一致：同一权重进出，见 ssr_composite.frag 的推导。
    // scene.a = 反射率 ρ（材质属性，与环境亮度无关），不是"镜面项占总亮度比例"。
    float rho = clamp(scene.a, 0.0, 1.0);
    float cover = clamp(ssr.a, 0.0, 1.0);
    float w = cover * rho * clamp(pc.env_fallback, 0.0, 1.0);
    if (w <= 0.0) {
        FragColor = vec4(scene.rgb, 1.0);
        return;
    }

    float scene_lum = dot(scene.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3  hue = scene_lum > 1e-4 ? scene.rgb / scene_lum : vec3(1.0);
    float refl_lum = dot(ssr.rgb, vec3(0.2126, 0.7152, 0.0722));

    FragColor = vec4(mix(scene.rgb, hue * refl_lum, w), 1.0);
}
