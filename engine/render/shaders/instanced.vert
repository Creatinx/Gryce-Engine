#version 430 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;

// 每实例的 model 矩阵（4 个 vec4，列主序拼回 mat4）。
// 绑定到 VAO 的 binding index 1，divisor = 1，按实例而非按顶点推进。
layout(location = 5) in vec4 aInstanceM0;
layout(location = 6) in vec4 aInstanceM1;
layout(location = 7) in vec4 aInstanceM2;
layout(location = 8) in vec4 aInstanceM3;

// GPU 剔除路径（cull_instances.comp）：实例矩阵来自剔除后的紧凑 SSBO，
// 用 gl_InstanceID 索引。uGpuCulled = 0 时走上面的顶点属性路径。
layout(std430, binding = 2) readonly buffer DstModels {
    mat4 dst_models[];
};

uniform int uGpuCulled; // 0 = 顶点属性实例流；1 = GPU 剔除后的 SSBO
// GPU 剔除路径下本批在 DstModels 里的起始下标。GL 的 gl_InstanceID 恒从 0 开始
// （glDrawArraysIndirect 忽略 baseInstance），而合并 dispatch 后各批数据是分段的，
// 必须显式加基址。Vulkan / D3D12 的实例索引已被 firstInstance / StartInstanceLocation
// 偏移（args 里的 baseInstance = 基址），这两个后端上该 uniform 恒为 0，不再叠加。
uniform int uGpuCulledBase;

out vec3 vFragPos;
out vec2 vTexCoord;
out vec3 vColor;
out mat3 vTBN;
out vec2 vScreenUV;

uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    mat4 model;
    if (uGpuCulled != 0) {
        model = dst_models[gl_InstanceID + uGpuCulledBase];
    } else {
        model = mat4(aInstanceM0, aInstanceM1, aInstanceM2, aInstanceM3);
    }

    vec4 world_pos = model * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    vec3 N = normalize(mat3(model) * aNormal);
    vec3 T = normalize(mat3(model) * aTangent);
    // Gram-Schmidt 正交化
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    gl_Position = uProjection * uView * world_pos;
    vScreenUV = (gl_Position.xy / gl_Position.w) * 0.5 + 0.5;
}