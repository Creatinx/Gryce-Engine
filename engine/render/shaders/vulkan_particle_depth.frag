#version 450 core

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

// 与 slot_to_binding(TextureSlots::kParticleTexture) 对应
layout(set = 0, binding = 1) uniform sampler2D uTexture;

// 深度补写 pass 专用：只决定"粒子在哪里算实体"，颜色由 (ZERO, ONE) 混合丢弃。
// 阈值远高于颜色 pass，避免把广告牌四边形的透明边缘写进深度缓冲——那会让
// SSR 射线撞上"看不见的墙"，在反射里采到背景色，形成一圈暗斑。
void main() {
    vec4 c = vColor * texture(uTexture, vUV);
    if (c.a <= 0.12) discard;
    FragColor = vec4(0.0);
}