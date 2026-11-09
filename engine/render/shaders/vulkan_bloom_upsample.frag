#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTextureA;
layout(binding = 1) uniform sampler2D uTextureB;

// 与 bloom_upsample.frag 同一常量与同一公式（着色器方言不同，效果必须一致）。
const float kChromaticDispersion = 0.004;

void main() {
    vec2 dir = vTexCoord - 0.5;
    vec2 disp = dir * kChromaticDispersion;

    vec3 c;
    c.r = texture(uTextureA, vTexCoord + disp).r;
    c.g = texture(uTextureA, vTexCoord).g;
    c.b = texture(uTextureA, vTexCoord - disp).b;
    c += texture(uTextureB, vTexCoord).rgb;
    FragColor = vec4(c, 1.0);
}
