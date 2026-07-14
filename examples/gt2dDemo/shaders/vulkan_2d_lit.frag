#version 450

#define MAX_LIGHTS 32

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec2 vNormalCoord;
layout(location = 3) in vec2 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;
layout(set = 0, binding = 1) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2DShadow uShadowMap;

struct Light {
    int type;
    vec2 pos;
    vec2 dir;
    vec3 color;
    float intensity;
    float radius;
    float range;
    float spot_angle;
    float spot_softness;
};

layout(set = 0, binding = 2) uniform LightUBO {
    vec3 uAmbient;
    int uLightCount;
    int uUseShadowMap;
    int uShadowLightIndex;
    vec2 uPad;
    mat4 uLightSpaceMatrix;
    Light uLights[MAX_LIGHTS];
} ubo;

float compute_shadow(vec4 light_space_pos) {
    vec3 proj = light_space_pos.xyz / light_space_pos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 0.0;
    return texture(uShadowMap, vec3(proj.xy, proj.z)).r;
}

void main() {
    vec4 albedo = texture(uAlbedo, vTexCoord) * vColor;
    if (albedo.a < 0.01) discard;

    vec3 normal = texture(uNormalMap, vNormalCoord).rgb;
    normal = normalize(normal * 2.0 - 1.0);
    if (normal.z < 0.0) normal.z = -normal.z;

    vec3 lit = albedo.rgb * ubo.uAmbient;

    for (int i = 0; i < ubo.uLightCount; ++i) {
        Light L = ubo.uLights[i];
        vec3 light_color = L.color * L.intensity;
        vec3 Lvec;
        float attenuation = 1.0;
        float spot_factor = 1.0;

        if (L.type == 0) {
            vec2 to_light = L.pos - vWorldPos;
            float dist = length(to_light);
            if (dist > L.radius) continue;
            attenuation = 1.0 - dist / L.radius;
            attenuation *= attenuation;
            Lvec = normalize(vec3(to_light, 0.15));
        } else if (L.type == 1) {
            Lvec = normalize(vec3(-L.dir, 0.15));
        } else {
            vec2 to_light = L.pos - vWorldPos;
            float dist = length(to_light);
            if (dist > L.range) continue;
            attenuation = 1.0 - dist / L.range;
            attenuation *= attenuation;
            Lvec = normalize(vec3(to_light, 0.15));
            vec2 spot_dir = normalize(L.dir);
            float cos_angle = dot(normalize(-to_light), spot_dir);
            float outer = cos(radians(L.spot_angle));
            float inner = cos(radians(L.spot_angle * (1.0 - L.spot_softness)));
            spot_factor = smoothstep(outer, inner, cos_angle);
            if (spot_factor <= 0.0) continue;
        }

        float diff = max(dot(normal, Lvec), 0.0);

        float shadow = 1.0;
        if (ubo.uUseShadowMap == 1 && ubo.uShadowLightIndex == i) {
            vec4 light_space = ubo.uLightSpaceMatrix * vec4(vWorldPos, 0.0, 1.0);
            shadow = compute_shadow(light_space);
        }

        lit += albedo.rgb * light_color * diff * attenuation * spot_factor * shadow;
    }

    outColor = vec4(lit, albedo.a);
}
