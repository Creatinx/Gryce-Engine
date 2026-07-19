#version 450 core

// Vulkan 蒙皮 PBR 顶点着色器：push constants 与 vulkan_pbr.vert 一致，
// palette 走 binding 8 的 UBO（与 VulkanShader skinned 管线的描述符布局对应）。

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;
layout(location = 5) in uvec4 aBoneIds;
layout(location = 6) in vec4 aBoneWeights;

layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

// 骨骼 palette：与 C++ render::k_max_skinning_bones 一致
layout(set = 0, binding = 8) uniform BonePalette {
    mat4 bones[128];
} uPalette;

layout(location = 0) out vec3 vFragPos;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec3 vColor;
layout(location = 3) out mat3 vTBN;
layout(location = 6) out vec4 vLightSpacePos;

void main() {
    // 线性混合蒙皮：Σ w_i * palette[id_i]；权重和 0 退化为单位阵
    float wsum = aBoneWeights.x + aBoneWeights.y + aBoneWeights.z + aBoneWeights.w;
    mat4 skin = mat4(1.0);
    if (wsum > 1e-6) {
        skin = aBoneWeights.x * uPalette.bones[aBoneIds.x]
             + aBoneWeights.y * uPalette.bones[aBoneIds.y]
             + aBoneWeights.z * uPalette.bones[aBoneIds.z]
             + aBoneWeights.w * uPalette.bones[aBoneIds.w];
    }

    vec4 world_pos = pc.uModel * skin * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    mat3 normal_mat = mat3(pc.uModel) * mat3(skin);
    vec3 N = normalize(normal_mat * aNormal);
    vec3 T = normalize(normal_mat * aTangent);
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    vLightSpacePos = pc.uLightSpaceMatrix * world_pos;
    gl_Position = pc.uProjection * pc.uView * world_pos;
}
