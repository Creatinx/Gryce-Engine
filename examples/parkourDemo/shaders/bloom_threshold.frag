#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uBloomThreshold;

void main() {
    // 4-tap 平均降采样到半分辨率，再按阈值提取亮部
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    vec3 c = texture(uTexture, vTexCoord).rgb;
    c += texture(uTexture, vTexCoord + vec2( texel.x, 0.0)).rgb;
    c += texture(uTexture, vTexCoord + vec2(0.0,  texel.y)).rgb;
    c += texture(uTexture, vTexCoord + texel).rgb;
    c *= 0.25;
    FragColor = vec4(max(c - uBloomThreshold, 0.0), 1.0);
}
