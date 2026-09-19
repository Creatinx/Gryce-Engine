#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    // 9-tap 高斯降采样（1,2,1 / 2,4,2 / 1,2,1）
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    vec3 c = texture(uTexture, vTexCoord).rgb * 4.0;
    c += texture(uTexture, vTexCoord + vec2( texel.x, 0.0)).rgb * 2.0;
    c += texture(uTexture, vTexCoord + vec2(-texel.x, 0.0)).rgb * 2.0;
    c += texture(uTexture, vTexCoord + vec2(0.0,  texel.y)).rgb * 2.0;
    c += texture(uTexture, vTexCoord + vec2(0.0, -texel.y)).rgb * 2.0;
    c += texture(uTexture, vTexCoord + vec2( texel.x,  texel.y)).rgb;
    c += texture(uTexture, vTexCoord + vec2(-texel.x,  texel.y)).rgb;
    c += texture(uTexture, vTexCoord + vec2( texel.x, -texel.y)).rgb;
    c += texture(uTexture, vTexCoord + vec2(-texel.x, -texel.y)).rgb;
    FragColor = vec4(c / 16.0, 1.0);
}
