#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outVSM;

// 材质 UBO（binding 0，std140，显式 offset 对齐 C++ UBOData）。
// 仅声明阴影 alpha test 所需字段：uEmissiveOpacity(+32, w=opacity)、uUseAlbedoMap(+96)。
layout(set = 0, binding = 0, std140) uniform MaterialUBO {
    layout(offset = 32) vec4 uEmissiveOpacity;
    layout(offset = 96) int uUseAlbedoMap;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;

void main() {
    float depth = gl_FragCoord.z;
    outVSM = vec4(depth, depth * depth, 0.0, 1.0);

    // Alpha Test
    if (ubo.uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * ubo.uEmissiveOpacity.w;
        if (alpha < 0.5) discard;
    }
}