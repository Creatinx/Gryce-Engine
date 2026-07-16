#version 450 core

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vColor;
layout(location = 3) in mat3 vTBN;
layout(location = 6) in vec4 vLightSpacePos;

layout(location = 0) out vec4 FragColor;

#define MAX_LIGHTS 8

struct Light {
    vec4 pos_type;        // xyz=position, w=type (0 方向光/1 点光/2 聚光)
    vec4 dir_range;       // xyz=direction, w=range
    vec4 color_intensity; // xyz=color, w=intensity
    vec4 spot;            // x=cos(outer), y=cos(inner)
};

// 与 C++ VulkanShader::UBOData 一一对应（std140）
layout(set = 0, binding = 0) uniform MaterialLightUBO {
    vec4 uAlbedoColor;
    vec4 uCameraPos;
    vec4 uEmissiveOpacity; // xyz=emissive, w=opacity
    vec4 uAmbient;         // xyz=环境光颜色
    vec4 uUVTransform;     // xy=scale, zw=offset
    float uRoughness;
    float uMetallic;
    float uAO;
    float uShadowBias;
    int uUseShadowMap;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseRoughnessMap;
    int uUseMetallicMap;
    int uUseAOMap;
    int uUseEmissiveMap;
    int uHDREnabled;       // 1=输出线性 HDR，0=内置 tonemap
    int uLightCount;
    int uShadowLightIndex; // 产生阴影的方向光下标（-1 表示无）
    int pad0;
    int pad1;
    Light uLights[MAX_LIGHTS];
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uRoughnessMap;
layout(set = 0, binding = 4) uniform sampler2D uMetallicMap;
layout(set = 0, binding = 5) uniform sampler2D uAOMap;
layout(set = 0, binding = 6) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 7) uniform sampler2D uEmissiveMap;

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
    if (ubo.uUseShadowMap == 0) return 1.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords = proj_coords * 0.5 + 0.5;

    if (proj_coords.z > 1.0) return 1.0;
    // 阴影贴图覆盖范围之外视为全亮（边缘 depth 被 clamp 会误判为阴影）
    if (proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) return 1.0;

    float current_depth = proj_coords.z;
    float bias = max(ubo.uShadowBias * (1.0 - dot(normal, light_dir)), ubo.uShadowBias * 0.1);

    vec2 texel_size = 1.0 / textureSize(uShadowMap, 0);
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 coords = vec3(proj_coords.xy + vec2(x, y) * texel_size, current_depth - bias);
            lit += texture(uShadowMap, coords);
        }
    }
    lit /= 9.0;

    return lit;
}

void main() {
    vec2 uv = vTexCoord * ubo.uUVTransform.xy + ubo.uUVTransform.zw;

    vec4 albedo_tex = texture(uAlbedoMap, uv);
    vec3 albedo = ubo.uUseAlbedoMap > 0 ? albedo_tex.rgb : ubo.uAlbedoColor.rgb;
    albedo *= vColor;
    float alpha = (ubo.uUseAlbedoMap > 0 ? albedo_tex.a : 1.0) * ubo.uEmissiveOpacity.w;

    vec3 normal = ubo.uUseNormalMap > 0
        ? normalize(texture(uNormalMap, uv).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);

    float roughness = ubo.uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : ubo.uRoughness;
    float metallic = ubo.uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : ubo.uMetallic;
    float ao = ubo.uUseAOMap > 0 ? texture(uAOMap, uv).r : ubo.uAO;

    vec3 V = normalize(ubo.uCameraPos.xyz - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < ubo.uLightCount; ++i) {
        Light light = ubo.uLights[i];
        int light_type = int(light.pos_type.w + 0.5);
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (light_type == 0) {
            // 方向光
            L = normalize(-light.dir_range.xyz);
            radiance = light.color_intensity.xyz * light.color_intensity.w;
            if (i == ubo.uShadowLightIndex) {
                shadow = shadow_calculation(vLightSpacePos, N, L);
            }
        } else {
            // 点光 / 聚光
            vec3 to_light = light.pos_type.xyz - vFragPos;
            float dist = length(to_light);
            float range = light.dir_range.w;
            if (dist > range) continue;
            L = to_light / dist;
            float attenuation = 1.0 - dist / range;
            attenuation *= attenuation;
            radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation;

            if (light_type == 2) {
                float cos_angle = dot(-L, normalize(light.dir_range.xyz));
                float spot = smoothstep(light.spot.x, light.spot.y, cos_angle);
                if (spot <= 0.0) continue;
                radiance *= spot;
            }
        }

        vec3 H = normalize(V + L);
        float NDF = distribution_ggx(N, H, roughness);
        float G = geometry_smith(N, V, L, roughness);
        vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    }

    vec3 ambient = ubo.uAmbient.rgb * albedo * ao;
    vec3 emissive = ubo.uEmissiveOpacity.xyz * (ubo.uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));

    vec3 color = ambient + Lo + emissive;

    if (ubo.uHDREnabled == 0) {
        // LDR 路径：内置 Reinhard + gamma（HDR 开启时由 tonemap pass 统一处理）
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }

    FragColor = vec4(color, alpha);
}
