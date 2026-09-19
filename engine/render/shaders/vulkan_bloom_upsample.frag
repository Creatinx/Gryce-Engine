#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTextureA;
layout(binding = 1) uniform sampler2D uTextureB;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uTextureA, 0));
    vec3 c = texture(uTextureA, vTexCoord + vec2( texel.x * 0.5,  texel.y * 0.5)).rgb;
    c += texture(uTextureA, vTexCoord + vec2(-texel.x * 0.5,  texel.y * 0.5)).rgb;
    c += texture(uTextureA, vTexCoord + vec2( texel.x * 0.5, -texel.y * 0.5)).rgb;
    c += texture(uTextureA, vTexCoord + vec2(-texel.x * 0.5, -texel.y * 0.5)).rgb;
    c *= 0.25;
    c += texture(uTextureB, vTexCoord).rgb;
    FragColor = vec4(c, 1.0);
}
