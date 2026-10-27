#version 330 core

in vec4 vCurrentPos;
in vec4 vPrevPos;
out vec4 FragColor;

// 渲染分辨率（像素）。NDC → 像素换算的基准，由 MV pass 传入。
uniform vec2 uRenderTargetSize;

// 输出：DLSS 像素空间运动向量（DLSS Programming Guide 3.6 节约定）：
// - 语义：把当前帧像素映射到它在上一帧的位置（prev - cur 方向），单位为像素
// - 坐标系：[0,0] 在左上；引擎 NDC 的 y 向上，因此 y 分量符号取反
// - 不做钳制：为防历史采样越界加的 clamp 会砍掉快速运动的真实矢量，
//   时域重建会因此拖影；越界防护由消费方的历史采样钳制负责
// 消费方：DLSS/FSR 类时域重建（P1 起）、TAA。
void main() {
    vec2 cur_ndc = vCurrentPos.xy / vCurrentPos.w;
    vec2 prev_ndc = vPrevPos.xy / vPrevPos.w;

    vec2 motion_px = vec2(prev_ndc.x - cur_ndc.x,
                          cur_ndc.y - prev_ndc.y) * 0.5 * uRenderTargetSize;

    FragColor = vec4(motion_px, 0.0, 1.0);
}
