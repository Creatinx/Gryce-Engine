#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。
uniform sampler2D uColorTex; // HDR 场景颜色（合成前）
uniform sampler2D uSSRTex;   // SSR 反射颜色（模糊后）

void main() {
    vec3 color = texture(uColorTex, vTexCoord).rgb;
    vec3 refl = texture(uSSRTex, vTexCoord).rgb;
    FragColor = vec4(color + refl, 1.0);
}
