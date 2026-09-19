#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] Depth+Normal 预通道输出：RGB = 世界法线 * 0.5 + 0.5，A = 粗糙度。
//        编码与 g_buffer.frag 的 RT1 及 ssr_trace.frag 的解码完全一致。

in vec3 vNormal;
in vec2 vTexCoord;

layout(location = 0) out vec4 gNormalRoughness;

uniform float uRoughness;
uniform float uOpacity;
uniform vec4 uUVTransform;
uniform int uUseAlbedoMap;
uniform int uUseRoughnessMap;
uniform int uTwoSided;
uniform sampler2D uAlbedoMap;
uniform sampler2D uRoughnessMap;

void main() {
    vec2 uv = vTexCoord * uUVTransform.xy + uUVTransform.zw;

    // Alpha 裁剪与 g_buffer 保持一致：镂空/带 alpha 的材质不该在法线缓冲里留实心法线
    if (uUseAlbedoMap != 0) {
        float alpha = texture(uAlbedoMap, uv).a * uOpacity;
        if (alpha < 0.5) discard;
    }

    vec3 N = normalize(vNormal);
    if (uTwoSided != 0 && !gl_FrontFacing) N = -N;
    float roughness = uUseRoughnessMap != 0 ? texture(uRoughnessMap, uv).r : uRoughness;

    gNormalRoughness = vec4(N * 0.5 + 0.5, roughness);
}
