#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
    vec2 uScreenSize;
    float uAmbient[3];
    float uPad0;
    vec2 uLightPos;
    float uLightRadius;
    float uLightIntensity;
    float uLightColor[3];
    float uPad1;
    float uPad2[2];
} pc;

void main() {
    vec4 albedo = texture(uAlbedo, vTexCoord) * vColor;
    if (albedo.a < 0.01) discard;

    vec3 ambient = vec3(pc.uAmbient[0], pc.uAmbient[1], pc.uAmbient[2]);
    vec3 lit = albedo.rgb * ambient;

    if (pc.uLightRadius > 0.0 && pc.uLightIntensity > 0.0) {
        vec2 fragPos = gl_FragCoord.xy;
        vec2 toLight = pc.uLightPos - fragPos;
        float dist = length(toLight);
        if (dist <= pc.uLightRadius) {
            float attenuation = 1.0 - dist / pc.uLightRadius;
            attenuation *= attenuation;
            vec3 lightColor = vec3(pc.uLightColor[0], pc.uLightColor[1], pc.uLightColor[2]);
            // 2D forward lighting：用法线 (0,0,1) 简化，光照方向带一点 Z 分量
            vec3 lightDir = normalize(vec3(toLight, 0.15));
            float diff = max(dot(vec3(0.0, 0.0, 1.0), lightDir), 0.0);
            lit += albedo.rgb * lightColor * diff * attenuation * pc.uLightIntensity;
        }
    }

    outColor = vec4(lit, albedo.a);
}
