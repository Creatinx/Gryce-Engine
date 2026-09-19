#version 330 core

in vec3 vNormal;
in vec3 vTangent;
in vec2 vTexCoord;
in vec3 vFragPos;
out vec4 FragColor;

// 材质参数
uniform float uRoughness;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform int uUseNormalMap;
uniform int uUseRoughnessMap;
uniform int uTwoSided;

// 输出：法线 (xyz) + 粗糙度 (w) 到 RGBA16F
void main() {
    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    vec3 normal = uUseNormalMap > 0
        ? normalize(texture(uNormalMap, vTexCoord).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    N = normalize(TBN * normal);
    if (uTwoSided != 0 && !gl_FrontFacing) N = -N;

    float roughness = uUseRoughnessMap > 0 ? texture(uRoughnessMap, vTexCoord).r : uRoughness;

    // 编码到 [0, 1] 范围
    FragColor = vec4(N * 0.5 + 0.5, roughness);
}