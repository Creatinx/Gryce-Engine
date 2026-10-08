#version 330 core
// ===========================================================================
// hiz_reduce.frag — 深度金字塔归约（OpenGL 方言，2x2 **max**）
// ===========================================================================
// 与 ssr_hiz.frag（2x2 min，服务屏幕空间反射）互为镜像：本文件为 GPU 遮挡剔除
// 服务，取的是"该区域**最远**的表面"——判断某个物体是否被整片遮挡时，需要的
// 正是"覆盖区域里最靠后的深度"，比它更近的物体才谈得上被挡住。
//
// 深度约定（全引擎统一）：窗口深度 w，near→0 / far→1，比较函数 LESS
// （gl_backend.cpp: glDepthFunc(GL_LESS)；vk_shader.cpp: VK_COMPARE_OP_LESS）。
// w 对距离单调递增，所以"最远"= max，无需线性化即可直接比较。
//
// 用 texelFetch + 整数纹素坐标（与 ssr_hiz 同理）：过滤/环绕模式不参与计算，
// GL 与 Vulkan 必然得到逐像素一致的金字塔。
//
// 输入既可以是主深度图（level 0 输入），也可以是上一级 HiZ（R32F）。
// ---------------------------------------------------------------------------
in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uInput;

void main() {
    ivec2 in_size = textureSize(uInput, 0);
    if (in_size.x <= 0 || in_size.y <= 0) {
        // 输入无效：写"最远"，即不遮挡任何东西（保守方向）。
        FragColor = vec4(1.0);
        return;
    }
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 base = dst * 2;
    ivec2 max_xy = in_size - ivec2(1);
    ivec2 c00 = clamp(base, ivec2(0), max_xy);
    ivec2 c10 = clamp(base + ivec2(1, 0), ivec2(0), max_xy);
    ivec2 c01 = clamp(base + ivec2(0, 1), ivec2(0), max_xy);
    ivec2 c11 = clamp(base + ivec2(1, 1), ivec2(0), max_xy);

    float d00 = texelFetch(uInput, c00, 0).r;
    float d10 = texelFetch(uInput, c10, 0).r;
    float d01 = texelFetch(uInput, c01, 0).r;
    float d11 = texelFetch(uInput, c11, 0).r;
    FragColor = vec4(max(max(d00, d10), max(d01, d11)));
}
