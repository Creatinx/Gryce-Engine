#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;

struct Light {
    vec2 pos;
    float radius;
    float intensity;
    vec3 color;
};

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
    vec2 uScreenSize;
    vec3 uAmbient;
    int uLightCount;
    Light uLights[4];
} pc;

void main() {
    vec4 albedo = texture(uAlbedo, vTexCoord) * vColor;
    if (albedo.a < 0.01) discard;

    vec3 lit = albedo.rgb * pc.uAmbient;

    vec2 fragPos = gl_FragCoord.xy;
    for (int i = 0; i < pc.uLightCount; ++i) {
        Light L = pc.uLights[i];
        if (L.radius <= 0.0 || L.intensity <= 0.0) continue;
        vec2 toLight = L.pos - fragPos;
        float dist = length(toLight);
        if (dist <= L.radius) {
            float attenuation = 1.0 - dist / L.radius;
            attenuation *= attenuation;
            // 2D forward lighting：用法线 (0,0,1) 简化，光照方向带一点 Z 分量
            vec3 lightDir = normalize(vec3(toLight, 0.15));
            float diff = max(dot(vec3(0.0, 0.0, 1.0), lightDir), 0.0);
            lit += albedo.rgb * L.color * diff * attenuation * L.intensity;
        }
    }

    outColor = vec4(lit, albedo.a);
}
