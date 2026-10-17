#version 430 core

// ===========================================================================
// webgl_instanced.vert — instanced.vert 的 WebGL2 变体
//
// 与 instanced.vert 的唯一区别：WebGL2 没有 SSBO，GPU 剔除后的紧凑实例
// 矩阵缓冲（DstModels，binding=2）整个不可用 —— 该路径在 Web 构建下由
// supports_compute=false 走 CPU 提交，实例矩阵一律来自顶点属性实例流
// （aInstanceM0..3，VAO binding 1，divisor=1）。
//
// 其余代码与 instanced.vert 逐行相同（版本行/精度由 ES300 转换器处理）。
// 解析顺序见 shader_source_resolver.cpp：WebGL 优先 webgl_ 前缀。
// ===========================================================================

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;

// 每实例的 model 矩阵（4 个 vec4，列主序拼回 mat4）。
// 绑定到 VAO 的 binding index 1，divisor = 1，按实例而非每顶点推进。
layout(location = 5) in vec4 aInstanceM0;
layout(location = 6) in vec4 aInstanceM1;
layout(location = 7) in vec4 aInstanceM2;
layout(location = 8) in vec4 aInstanceM3;

// 桌面 GL 上这两个 uniform 选择 SSBO 实例路径；WebGL2 无 SSBO，保留声明
// 只为让引擎侧的 uniform 设置代码路径不变（未使用 → 被优化掉，
// glGetUniformLocation 返回 -1，glUniform*(-1) 按规范静默忽略）。
uniform int uGpuCulled;
uniform int uGpuCulledBase;

out vec3 vFragPos;
out vec2 vTexCoord;
out vec3 vColor;
out mat3 vTBN;
out vec2 vScreenUV;

uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    // WebGL2：实例矩阵只走顶点属性流（GPU 剔除/SSBO 路径不可用）。
    mat4 model = mat4(aInstanceM0, aInstanceM1, aInstanceM2, aInstanceM3);

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
