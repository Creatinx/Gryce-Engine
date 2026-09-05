#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;

void main() {
    float alpha = texture(uFontAtlas, vTexCoord).r;
    float a = smoothstep(0.45, 0.55, alpha);
    if (a < 0.01) discard;
    outColor = vec4(vColor.rgb, vColor.a * a);
}
