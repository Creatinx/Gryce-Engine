#version 330 core

in vec4 vCurrentPos;
in vec4 vPrevPos;
out vec4 FragColor;

// 输出：屏幕空间运动向量 (xy) 到 RGBA16F
// 编码为 NDC 空间位移，TAA 用此 reproject 历史帧
void main() {
    vec2 cur_ndc = vCurrentPos.xy / vCurrentPos.w;
    vec2 prev_ndc = vPrevPos.xy / vPrevPos.w;

    // 计算 NDC 空间位移（[-1,1] 范围），映射到 [0,1] 存储
    vec2 motion = (cur_ndc - prev_ndc) * 0.5;

    // 钳制极端运动，防止 TAA 采样越界
    motion = clamp(motion, -0.1, 0.1);

    FragColor = vec4(motion, 0.0, 1.0);
}