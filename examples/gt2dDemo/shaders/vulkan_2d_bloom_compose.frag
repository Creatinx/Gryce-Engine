#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBloom;

layout(push_constant, std430) uniform PushConstants {
    float uIntensity;
} pc;

void main() {
    vec3 scene = texture(uScene, vTexCoord).rgb;
    vec3 bloom = texture(uBloom, vTexCoord).rgb;
    outColor = vec4(scene + bloom * pc.uIntensity, 1.0);
}
