#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
} pc;

// 用 specialization constant 区分 rect/text，避免某些驱动下 push constant int 在 fragment stage 失效
layout(constant_id = 0) const int USE_TEXTURE = 0;

void main() {
    if (USE_TEXTURE != 0) {
        float alpha = texture(uFontAtlas, vTexCoord).r;
        float a = smoothstep(0.45, 0.55, alpha);
        if (a < 0.01) discard;
        outColor = vec4(vColor.rgb, vColor.a * a);
    } else {
        outColor = vColor;
    }
}
