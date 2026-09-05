#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uHDRTexture;     // 当前帧（已抖动）
uniform sampler2D uHistoryTexture; // 上一帧结果
uniform float uTAAWeight = 0.85;
uniform int uTAAEnabled = 1;

void main() {
    vec3 current = texture(uHDRTexture, vTexCoord).rgb;
    if (uTAAEnabled == 0) {
        FragColor = vec4(current, 1.0);
        return;
    }
    vec3 history = texture(uHistoryTexture, vTexCoord).rgb;

    // 邻域钳制：以当前帧 3x3 的 min/max 约束历史，抑制闪烁/拖影
    vec2 texel = 1.0 / vec2(textureSize(uHDRTexture, 0));
    vec3 mn = current;
    vec3 mx = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 c = texture(uHDRTexture, vTexCoord + vec2(float(x), float(y)) * texel).rgb;
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    vec3 clamped = clamp(history, mn, mx);
    FragColor = vec4(mix(current, clamped, uTAAWeight), 1.0);
}
