#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTexture;      // 平均亮度（1x1）
uniform sampler2D uPrevExposure; // 上一帧曝光（1x1）
uniform float uAETargetLuminance;
uniform float uAEMinExposure;
uniform float uAEMaxExposure;
uniform float uAESpeed;

void main() {
    float avg = max(texture(uTexture, vec2(0.5)).r, 1e-4);
    float prev = texture(uPrevExposure, vec2(0.5)).r;
    float target = clamp(uAETargetLuminance / avg, uAEMinExposure, uAEMaxExposure);
    float exposure = mix(prev, target, clamp(uAESpeed, 0.0, 1.0));
    FragColor = vec4(exposure, exposure, exposure, 1.0);
}
