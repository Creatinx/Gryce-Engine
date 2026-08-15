#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTexture;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    vec3 c = texture(uTexture, vTexCoord).rgb;
    c += texture(uTexture, vTexCoord + vec2(texel.x, 0.0)).rgb;
    c += texture(uTexture, vTexCoord + vec2(0.0, texel.y)).rgb;
    c += texture(uTexture, vTexCoord + texel).rgb;
    c *= 0.25;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    FragColor = vec4(lum, lum, lum, 1.0);
}
