#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// 材质 UBO：alpha test 所需字段
layout(set = 0, binding = 0, std140) uniform MaterialUBO {
    layout(offset = 32) vec4 uEmissiveOpacity;
    layout(offset = 96) int uUseAlbedoMap;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;

void main() {
    // Alpha Test：透明像素丢弃
    if (ubo.uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * ubo.uEmissiveOpacity.w;
        if (alpha < 0.5) discard;
    }
    // 输出深度到颜色附件（RGBA16F 存储深度值，供 PCF 采样）
    FragColor = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}