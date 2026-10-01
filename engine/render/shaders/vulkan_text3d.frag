#version 450 core

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

// 与 slot_to_binding(TextureSlots::kParticleTexture) 对应（PBR albedo 的 binding 1）。
// 文本 pass 与 PBR pass 不同时执行，复用该 binding 不冲突。
layout(set = 0, binding = 1) uniform sampler2D uTexture;

void main() {
    // 字形图集是 RGBA8：RGB 恒为白、A 为字形覆盖度（见 FontAtlas 的打包方式）。
    vec4 c = texture(uTexture, vUV);
    c.rgb *= vColor.rgb;
    c.a *= vColor.a;
    if (c.a <= 0.002) discard;
    FragColor = c;
}