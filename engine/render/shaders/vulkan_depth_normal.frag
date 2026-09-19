#version 450 core

// [Shader 阶段] Fragment Shader（Vulkan）
// [功能] Depth+Normal 预通道输出：RGB = 世界法线 * 0.5 + 0.5，A = 粗糙度。
//        编码与 vulkan g_buffer / ssr_trace 的解码一致。

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 gNormalRoughness;

// 与 C++ VulkanShader::UBOData 严格对齐（std140）。
// 只用到前若干字段，但必须逐字段照抄前缀，否则偏移对不上（std140 偏移由
// 前面的成员决定）。整块布局与 vulkan_pbr.frag 的 MaterialLightUBO 前缀一致。
layout(set = 0, binding = 0) uniform MaterialLightUBO {
    vec4 uAlbedoColor;
    vec4 uCameraPos;
    vec4 uEmissiveOpacity; // xyz=emissive, w=opacity
    vec4 uAmbient;
    vec4 uUVTransform;
    float uRoughness;
    float uMetallic;
    float uAO;
    int uUseShadowMap;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseRoughnessMap;
    int uUseMetallicMap;
    int uUseAOMap;
    int uUseEmissiveMap;
    int uHDREnabled;
    int uLightCount;
    int uShadowLightIndex;
    int uUseIBL;
    float uIBLIntensity;
    int uTwoSided;
    vec4 _pad_std140;
} ubo;

// 采样器绑定槽位与 vulkan_pbr.frag 相同（引擎按 TextureSlots 固定映射）
layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;
layout(set = 0, binding = 3) uniform sampler2D uRoughnessMap;

void main() {
    vec2 uv = vTexCoord * ubo.uUVTransform.xy + ubo.uUVTransform.zw;

    if (ubo.uUseAlbedoMap != 0) {
        float alpha = texture(uAlbedoMap, uv).a * ubo.uEmissiveOpacity.w;
        if (alpha < 0.5) discard;
    }

    vec3 N = normalize(vNormal);
    if (ubo.uTwoSided != 0 && !gl_FrontFacing) N = -N;
    float roughness = ubo.uUseRoughnessMap != 0 ? texture(uRoughnessMap, uv).r : ubo.uRoughness;

    gNormalRoughness = vec4(N * 0.5 + 0.5, roughness);
}
