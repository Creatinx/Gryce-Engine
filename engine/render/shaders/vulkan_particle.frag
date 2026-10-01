#version 450 core

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

// 与 slot_to_binding(TextureSlots::kParticleTexture) 对应（PBR albedo 的 binding 1）。
// 粒子 pass 与 PBR pass 不同时执行，复用该 binding 不冲突。
layout(set = 0, binding = 1) uniform sampler2D uTexture;

void main() {
    // 无贴图时绑定 1x1 白色回退贴图，统一按"贴图 × 顶点色"处理。
    vec4 c = vColor * texture(uTexture, vUV);
    if (c.a <= 0.002) discard;
    FragColor = c;
}