#version 330 core

// 从深度重建世界位置，计算屏幕空间运动向量
// 输出：RG16F — (motion_x, motion_y) 在 NDC 空间

in vec2 vUV;
layout(location = 0) out vec2 FragColor;

uniform sampler2D uDepthTex;
uniform vec2 uScreenSize = vec2(1280.0, 720.0);

// 逆投影矩阵：将 NDC 转换到视图空间
uniform mat4 uInvProj = mat4(1.0);
// 逆视图矩阵：将视图空间转换到世界空间
uniform mat4 uInvView = mat4(1.0);
// 前一帧的视图-投影矩阵
uniform mat4 uPrevViewProj = mat4(1.0);
// 当前帧的视图-投影矩阵
uniform mat4 uCurrViewProj = mat4(1.0);

// 从深度重建 NDC 坐标
float linearize_depth(float d, float near, float far) {
    return (2.0 * near) / (far + near - d * (far - near));
}

void main() {
    float depth = texture(uDepthTex, vUV).r;

    // 重建 NDC 坐标（范围 [-1, 1]）
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);

    // 逆投影到视图空间
    vec4 view_pos = uInvProj * ndc;
    view_pos /= view_pos.w;

    // 逆视图到世界空间
    vec4 world_pos = uInvView * view_pos;
    world_pos /= world_pos.w;

    // 前一帧投影到 NDC
    vec4 prev_ndc = uPrevViewProj * world_pos;
    prev_ndc /= prev_ndc.w;

    // 当前帧投影到 NDC
    vec4 curr_ndc = uCurrViewProj * world_pos;
    curr_ndc /= curr_ndc.w;

    // 计算 NDC 空间运动向量（范围 [-1, 1]），映射到 [0, 1] 存储
    vec2 motion = (curr_ndc.xy - prev_ndc.xy) * 0.5;

    // 钳制极端运动
    motion = clamp(motion, -0.1, 0.1);

    FragColor = motion;
}