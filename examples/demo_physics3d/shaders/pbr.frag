#version 330 core

in vec3 vFragPos;
in vec2 vTexCoord;
in vec3 vColor;
in mat3 vTBN;
in vec4 vLightSpacePos;

out vec4 FragColor;

// ---------------------------------------------------------------------------
// 材质
// ---------------------------------------------------------------------------
uniform vec3 uAlbedoColor;
uniform float uRoughness;
uniform float uMetallic;
uniform float uAO;

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform sampler2D uMetallicMap;
uniform sampler2D uAOMap;
uniform sampler2DShadow uShadowMap;

uniform int uUseAlbedoMap;
uniform int uUseNormalMap;
uniform int uUseRoughnessMap;
uniform int uUseMetallicMap;
uniform int uUseAOMap;

// ---------------------------------------------------------------------------
// 灯光 / 相机
// ---------------------------------------------------------------------------
uniform vec3 uCameraPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uLightIntensity;

uniform float uShadowBias;
uniform int uUseShadowMap;

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR 辅助函数
// ---------------------------------------------------------------------------
float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom + 0.0001);
}

float geometry_schlick_ggx(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometry_schlick_ggx(max(dot(N, V), 0.0), roughness) *
           geometry_schlick_ggx(max(dot(N, L), 0.0), roughness);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cos_theta, 5.0);
}

// ---------------------------------------------------------------------------
// Shadow
// ---------------------------------------------------------------------------
float shadow_calculation(vec4 light_space_pos, vec3 normal, vec3 light_dir) {
    if (uUseShadowMap == 0) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords = proj_coords * 0.5 + 0.5;

    if (proj_coords.z > 1.0) return 0.0;

    float current_depth = proj_coords.z;
    float bias = max(uShadowBias * (1.0 - dot(normal, light_dir)), uShadowBias * 0.1);

    // 硬件 PCF：sampler2DShadow + 线性过滤，自动做 2x2 百分比渐近过滤
    vec2 texel_size = 1.0 / textureSize(uShadowMap, 0);
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 coords = vec3(proj_coords.xy + vec2(x, y) * texel_size, current_depth - bias);
            shadow += texture(uShadowMap, coords);
        }
    }
    shadow /= 9.0;

    return shadow;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main() {
    vec3 albedo = uUseAlbedoMap > 0 ? texture(uAlbedoMap, vTexCoord).rgb : uAlbedoColor;

    vec3 normal = uUseNormalMap > 0
        ? normalize(texture(uNormalMap, vTexCoord).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);

    float roughness = uUseRoughnessMap > 0 ? texture(uRoughnessMap, vTexCoord).r : uRoughness;
    float metallic = uUseMetallicMap > 0 ? texture(uMetallicMap, vTexCoord).r : uMetallic;
    float ao = uUseAOMap > 0 ? texture(uAOMap, vTexCoord).r : uAO;

    vec3 V = normalize(uCameraPos - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 L = normalize(-uLightDir);
    vec3 H = normalize(V + L);
    vec3 radiance = uLightColor * uLightIntensity;

    float NDF = distribution_ggx(N, H, roughness);
    float G = geometry_smith(N, V, L, roughness);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    float NdotL = max(dot(N, L), 0.0);
    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    vec3 ambient = vec3(0.15) * albedo * ao;

    // Shadow：只衰减直接光，保持环境光可见
    float shadow = shadow_calculation(vLightSpacePos, N, L);
    vec3 color = ambient + Lo * shadow;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
