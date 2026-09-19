#version 450 core

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

// 骨骼 palette：与 C++ palette_stride_（128 * mat4）一致
layout(set = 0, binding = 8) uniform PaletteUBO {
    mat4 uBonePalette[128];
} palette;

layout(location = 0) out vec3 vFragPos;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec3 vColor;
layout(location = 3) out mat3 vTBN;
layout(location = 6) out vec4 vLightSpacePos;
layout(location = 7) out vec2 vScreenUV;

void main() {
    float wsum = aBoneWeights.x + aBoneWeights.y + aBoneWeights.z + aBoneWeights.w;
    mat4 skin = mat4(1.0);
    if (wsum > 1e-6) {
        skin = aBoneWeights.x * palette.uBonePalette[aBoneIds.x]
             + aBoneWeights.y * palette.uBonePalette[aBoneIds.y]
             + aBoneWeights.z * palette.uBonePalette[aBoneIds.z]
             + aBoneWeights.w * palette.uBonePalette[aBoneIds.w];
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
    vScreenUV = (gl_Position.xy / gl_Position.w) * 0.5 + 0.5;
    vScreenUV.y = 1.0 - vScreenUV.y;
}
