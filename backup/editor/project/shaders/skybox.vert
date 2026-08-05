#version 330 core

layout(location = 0) in vec3 aPos;

out vec3 vDir;

uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    vDir = aPos;
    // 深度恒为远平面（w = z），配合 LEQUAL/关深度测试绘制背景
    vec4 pos = uProjection * uView * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
