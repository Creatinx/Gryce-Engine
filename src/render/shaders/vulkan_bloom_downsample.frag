#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTexture;

void main() {
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
