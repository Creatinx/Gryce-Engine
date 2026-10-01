#version 330 core

in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;

out vec4 FragColor;

// 深度补写 pass 专用片元着色器：只决定"粒子在哪里算实体"，不输出颜色
// （颜色由 draw_particles_depth 的 (ZERO, ONE) 混合丢弃）。
//
// 为什么需要单独一份而不是复用 particle.frag：广告牌四边形比看得见的粒子大。
// 径向衰减贴图让边缘近乎全透明，颜色 pass 里看不见，但深度 pass 若照样写深度，
// SSR 的屏幕空间步进会撞上这圈"看不见的墙"，在反射里采到该处的背景色，
// 于是金属面上出现一块比粒子本身大一圈的暗斑。这里用远高于颜色 pass 的阈值
// 把深度足迹收缩到真正可见的部分。
void main() {
    vec4 c = vColor * texture(uTexture, vUV);
    if (c.a <= 0.12) discard;
    FragColor = vec4(0.0);
}