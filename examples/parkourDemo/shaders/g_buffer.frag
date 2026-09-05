#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] GBuffer 填充 Pass：将材质属性写入 MRT
// [输入] 顶点着色器传递的 vFragPos/vTexCoord/vColor/vNormal，材质参数
// [输出]
//   RT0: Albedo (RGB) + Metallic (A)  — RGBA8
//   RT1: Normal (RGB) + Roughness (A)  — RGBA16F
//   RT2: Emissive (RGB) + AO (A)       — RGBA16F

in vec3 vFragPos;
in vec2 vTexCoord;
in vec3 vColor;
in vec3 vNormal;

layout(location = 0) out vec4 gAlbedoMetallic;
layout(location = 1) out vec4 gNormalRoughness;
layout(location = 2) out vec4 gEmissiveAO;

uniform vec3 uAlbedoColor;
uniform float uRoughness;
uniform float uMetallic;
uniform float uAO;
uniform vec3 uEmissiveColor;
uniform vec4 uUVTransform;

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform sampler2D uMetallicMap;
uniform sampler2D uAOMap;
uniform sampler2D uEmissiveMap;

uniform int uUseAlbedoMap;
uniform int uUseNormalMap;
uniform int uUseRoughnessMap;
uniform int uUseMetallicMap;
uniform int uUseAOMap;
uniform int uUseEmissiveMap;
uniform int uTwoSided;

uniform float uOpacity;

void main() {
    vec2 uv = vTexCoord * uUVTransform.xy + uUVTransform.zw;

    // Albedo + Metallic
    vec4 albedo_tex = texture(uAlbedoMap, uv);
    vec3 albedo = uUseAlbedoMap > 0 ? albedo_tex.rgb : uAlbedoColor;
    albedo *= vColor;
    float metallic = uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : uMetallic;
    // Alpha 裁剪
    float alpha = (uUseAlbedoMap > 0 ? albedo_tex.a : 1.0) * uOpacity;
    if (alpha < 0.5) discard;
    gAlbedoMetallic = vec4(albedo, metallic);

    // Normal + Roughness
    vec3 normal = uUseNormalMap > 0
        ? normalize(texture(uNormalMap, uv).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vNormal);  // 世界空间法线，法线贴图需要 TBN 变换
    // 简化：使用几何法线，法线贴图需要 TBN 矩阵（从顶点着色器传入）
    if (uTwoSided != 0 && !gl_FrontFacing) N = -N;
    float roughness = uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : uRoughness;
    gNormalRoughness = vec4(N * 0.5 + 0.5, roughness);

    // Emissive + AO
    vec3 emissive = uEmissiveColor * (uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));
    float ao = uUseAOMap > 0 ? texture(uAOMap, uv).r : uAO;
    gEmissiveAO = vec4(emissive, ao);

    // 深度由 OpenGL 自动写入
}