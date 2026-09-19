#version 330 core
// ESM: 输出指数深度 exp(-c * depth)

in vec2 vTexCoord;

uniform sampler2D uAlbedoMap;
uniform int uUseAlbedoMap;
uniform float uOpacity;
uniform float uESMExponent; // 默认 80.0

out float outESM;

void main() {
    float depth = gl_FragCoord.z;
    outESM = exp(-uESMExponent * depth);

    // Alpha Test
    if (uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * uOpacity;
        if (alpha < 0.5) discard;
    }
}