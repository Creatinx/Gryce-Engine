#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    float exposure;
    float ev100;
    int mode;
    int dithering;
    float white_point;
    float black_point;
    float contrast;
    float saturation;
    vec4 lift;
    vec4 gamma;
    vec4 gain;
    vec4 shadows;
    vec4 midtones;
    vec4 highlights;
    int bloom_enabled;
    float bloom_threshold;
    float bloom_intensity;
    float film_grain;
    float vignette;
    float chromatic_aberration;
    float _pad0;
    float _pad1;
} pc;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    vec3 c = texture(uTexture, vTexCoord).rgb;
    c += texture(uTexture, vTexCoord + vec2( texel.x, 0.0)).rgb;
    c += texture(uTexture, vTexCoord + vec2(0.0,  texel.y)).rgb;
    c += texture(uTexture, vTexCoord + texel).rgb;
    c *= 0.25;
    FragColor = vec4(max(c - pc.bloom_threshold, 0.0), 1.0);
}
