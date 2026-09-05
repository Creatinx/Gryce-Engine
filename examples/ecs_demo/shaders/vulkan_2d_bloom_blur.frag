#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant, std430) uniform PushConstants {
    vec2 uDirection;
    vec2 uTexelSize;
} pc;

void main() {
    vec2 off1 = vec2(1.3846153846) * pc.uDirection * pc.uTexelSize;
    vec2 off2 = vec2(3.2307692308) * pc.uDirection * pc.uTexelSize;
    outColor = texture(uTexture, vTexCoord) * 0.2270270270;
    outColor += texture(uTexture, vTexCoord + off1) * 0.3162162162;
    outColor += texture(uTexture, vTexCoord - off1) * 0.3162162162;
    outColor += texture(uTexture, vTexCoord + off2) * 0.0702702703;
    outColor += texture(uTexture, vTexCoord - off2) * 0.0702702703;
}
