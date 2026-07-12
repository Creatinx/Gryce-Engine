#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
    vec2 uScreenSize;
    vec3 uAmbient;
    vec2 uLightPos;
    float uLightRadius;
    float uLightIntensity;
    vec3 uLightColor;
} pc;

void main() {
    vec4 albedo = texture(uAlbedo, vTexCoord) * vColor;
    if (albedo.a < 0.01) discard;

    vec3 lit = albedo.rgb * pc.uAmbient;

    if (pc.uLightRadius > 0.0 && pc.uLightIntensity > 0.0) {
        vec2 fragPos = gl_FragCoord.xy;
        vec2 toLight = pc.uLightPos - fragPos;
        float dist = length(toLight);
        if (dist <= pc.uLightRadius) {
            float attenuation = 1.0 - dist / pc.uLightRadius;
            attenuation *= attenuation;
            vec3 lightDir = normalize(vec3(toLight, 0.15));
            float diff = max(dot(vec3(0.0, 0.0, 1.0), lightDir), 0.0);
            lit += albedo.rgb * pc.uLightColor * diff * attenuation * pc.uLightIntensity;
        }
    }

    outColor = vec4(lit, albedo.a);
}
