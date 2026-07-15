#version 450 core

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vColor;
layout(location = 3) in mat3 vTBN;
layout(location = 6) in vec4 vLightSpacePos;

layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform MaterialLightUBO {
    vec4 uAlbedoColor;
    vec4 uCameraPos;
    vec4 uLightDir;
    vec4 uLightColor;
    float uRoughness;
    float uMetallic;
    float uAO;
    float uShadowBias;
    float uLightIntensity;
    int uUseShadowMap;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseRoughnessMap;
    int uUseMetallicMap;
    int uUseAOMap;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uRoughnessMap;
layout(set = 0, binding = 4) uniform sampler2D uMetallicMap;
layout(set = 0, binding = 5) uniform sampler2D uAOMap;
layout(set = 0, binding = 6) uniform sampler2DShadow uShadowMap;

const float PI = 3.14159265359;

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

float shadow_calculation(vec4 light_space_pos, vec3 normal, vec3 light_dir) {
    if (ubo.uUseShadowMap == 0) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords = proj_coords * 0.5 + 0.5;

    if (proj_coords.z > 1.0) return 0.0;

    float current_depth = proj_coords.z;
    float bias = max(ubo.uShadowBias * (1.0 - dot(normal, light_dir)), ubo.uShadowBias * 0.1);

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

void main() {
    vec3 albedo = ubo.uUseAlbedoMap > 0 ? texture(uAlbedoMap, vTexCoord).rgb : ubo.uAlbedoColor.rgb;
    albedo *= vColor;

    vec3 normal = ubo.uUseNormalMap > 0
        ? normalize(texture(uNormalMap, vTexCoord).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);

    float roughness = ubo.uUseRoughnessMap > 0 ? texture(uRoughnessMap, vTexCoord).r : ubo.uRoughness;
    float metallic = ubo.uUseMetallicMap > 0 ? texture(uMetallicMap, vTexCoord).r : ubo.uMetallic;
    float ao = ubo.uUseAOMap > 0 ? texture(uAOMap, vTexCoord).r : ubo.uAO;

    vec3 V = normalize(ubo.uCameraPos.xyz - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 L = normalize(-ubo.uLightDir.xyz);
    vec3 H = normalize(V + L);
    vec3 radiance = ubo.uLightColor.rgb * ubo.uLightIntensity;

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

    // Shadow：仅衰减直接光，保持环境光可见
    float shadow = shadow_calculation(vLightSpacePos, N, L);
    // shadow=1 表示被照亮，shadow=0 表示在阴影中
    vec3 color = ambient + Lo * shadow;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
