#version 330 core
in vec2 vTexCoord;
uniform sampler2D uAlbedoMap;
uniform int uUseAlbedoMap;
uniform float uOpacity;
out vec4 FragColor;
void main() {
    // Alpha Test: 透明像素丢弃
    if (uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * uOpacity;
        if (alpha < 0.5) discard;
    }
    // 输出深度到颜色附件（RGBA16F 存储深度值，供 PCF 采样）
    FragColor = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}