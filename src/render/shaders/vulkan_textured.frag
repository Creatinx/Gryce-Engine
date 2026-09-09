#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vNormal;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAlbedoMap;

void main() {
    vec3 albedo = texture(uAlbedoMap, vTexCoord).rgb;
    vec3 normal = normalize(vNormal);
    vec3 light_dir = normalize(vec3(0.0, -0.3, -1.0));
    float ndotl = max(dot(normal, -light_dir), 0.0);
    vec3 ambient = albedo * 0.15;
    vec3 lit = albedo * ndotl;
    outColor = vec4(ambient + lit, 1.0);
}
