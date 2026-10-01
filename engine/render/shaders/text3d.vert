#version 330 core

// 3D 文本（TextMesh3D）顶点着色器
// 顶点已经是组件在 CPU 侧按字形图集展开的局部空间字形四边形
// （position(3)+uv(2)+color(4)），这里只做 MVP 变换，不需要任何展开计算。
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * uView * uModel * vec4(aPosition, 1.0);
}