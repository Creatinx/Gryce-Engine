#version 450 core

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 uView;
    mat4 uProjection;
} pc;

layout(location = 0) out vec3 vDir;

void main() {
    vDir = aPos;
    // 深度恒为远平面（w = z），配合 LESS_OR_EQUAL 绘制背景
    vec4 pos = pc.uProjection * pc.uView * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
