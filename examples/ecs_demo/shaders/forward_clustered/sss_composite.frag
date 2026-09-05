#version 330 core

// SSS 合成
// 将 SSS 模糊结果与原始场景颜色混合

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uSSSBlur;

uniform float uSSSStrength;

void main() {
    vec4 original = texture(uSceneColor, vUV);
    vec4 sss = texture(uSSSBlur, vUV);

    float sss_weight = original.a;

    // 仅在 SSS 强度 > 0 的区域混合
    vec3 result = mix(original.rgb, sss.rgb, sss_weight * uSSSStrength);

    FragColor = vec4(result, 1.0);
}