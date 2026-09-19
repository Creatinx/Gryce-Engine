#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTextureA; // 更小的上一级（上采样模糊）
uniform sampler2D uTextureB; // 当前级（保留自身贡献）

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
