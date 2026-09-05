#version 330 core
// VSM: 输出深度和深度²
layout(location = 0) out vec2 outVSM;

in vec2 vTexCoord;

uniform sampler2D uAlbedoMap;
uniform int uUseAlbedoMap;
uniform float uOpacity;

void main() {
    float depth = gl_FragCoord.z;
    outVSM = vec2(depth, depth * depth);

    // Alpha Test
    if (uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * uOpacity;
        if (alpha < 0.5) discard;
    }
}