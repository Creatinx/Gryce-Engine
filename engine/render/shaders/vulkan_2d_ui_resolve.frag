#version 450

// 覆盖层超采样解析：把 2x 尺寸的 UI 离屏目标降采样到 back buffer。
// 采样器是线性的，输出像素与输入像素正好 2:1 对齐，双线性在四 texel 交界处
// 权重各 0.25，等价于 2x2 盒式平均 —— 不需要额外的多 tap 卷积。

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {
    // 覆盖层离屏目标与 back buffer 的 Y 朝向相反（两边都用翻转 viewport，
    // 相当于多翻了一次），这里翻回来。
    vec4 c = texture(uTexture, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
    // 离屏目标是清成透明黑再画上去的，混合后 RGB 已经被 alpha 预乘过。解析时
    // 若直接按非预乘再做一次 alpha 混合，边缘就被乘了两次 alpha，整块字发淡。
    // 这里先反预乘还原成直通颜色，交给固定的 Alpha 混合。
    outColor = vec4(c.a > 0.0001 ? c.rgb / c.a : vec3(0.0), c.a);
}
