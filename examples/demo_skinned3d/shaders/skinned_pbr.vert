#version 330 core

// 蒙皮 PBR 顶点着色器：在 pbr.vert 基础上追加骨骼权重属性与 palette LBS。
// location 5 = bone ids（uvec4，整数属性），location 6 = weights（vec4）。

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;
layout(location = 5) in uvec4 aBoneIds;
layout(location = 6) in vec4 aBoneWeights;

out vec3 vFragPos;
out vec2 vTexCoord;
out vec3 vColor;
out mat3 vTBN;
out vec4 vLightSpacePos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpaceMatrix;

// 骨骼 palette：与 C++ render::k_max_skinning_bones 一致
const int MAX_BONES = 128;
uniform mat4 uBonePalette[MAX_BONES];

void main() {
    // 线性混合蒙皮：Σ w_i * palette[id_i]。
    // 权重和为 0（未绑定顶点）时退化为单位阵，顶点保持原位。
    float wsum = aBoneWeights.x + aBoneWeights.y + aBoneWeights.z + aBoneWeights.w;
    mat4 skin = mat4(1.0);
    if (wsum > 1e-6) {
        skin = aBoneWeights.x * uBonePalette[aBoneIds.x]
             + aBoneWeights.y * uBonePalette[aBoneIds.y]
             + aBoneWeights.z * uBonePalette[aBoneIds.z]
             + aBoneWeights.w * uBonePalette[aBoneIds.w];
    }

    vec4 world_pos = uModel * skin * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    mat3 normal_mat = mat3(uModel) * mat3(skin);
    vec3 N = normalize(normal_mat * aNormal);
    vec3 T = normalize(normal_mat * aTangent);
    // Gram-Schmidt 正交化
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    vLightSpacePos = uLightSpaceMatrix * world_pos;
    gl_Position = uProjection * uView * world_pos;
}
