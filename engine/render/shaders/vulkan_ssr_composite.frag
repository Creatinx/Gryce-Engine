#version 450 core

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 0 = uColorTex (kTonemapHDR), 10 = uSSRTex (kSSRTexture)
layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 10) uniform sampler2D uSSRTex;

void main() {
    vec3 color = texture(uColorTex, vUV).rgb;
    vec3 refl = texture(uSSRTex, vUV).rgb;
    FragColor = vec4(color + refl, 1.0);
}